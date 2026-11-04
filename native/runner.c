#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Private wire format: one JSON metadata line, followed by up to 16000 raw bytes.
 * No model/network logic lives here. The Python caller owns approval and env. */
static volatile sig_atomic_t stopped = 0;

static void on_signal(int number) { stopped = number; }

static long long millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: desk-runner WORKSPACE TIMEOUT_MS COMMAND\n");
        return 2;
    }
    char *end = NULL;
    errno = 0;
    long timeout = strtol(argv[2], &end, 10);
    if (errno || !end || *end || timeout < 1 || timeout > 300000) {
        fprintf(stderr, "invalid timeout\n");
        return 2;
    }
    FILE *output = tmpfile();
    if (!output) { perror("tmpfile"); return 2; }
    int output_fd = fileno(output);
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0) { perror("open"); fclose(output); return 2; }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction"); close(null_fd); fclose(output); return 2;
    }
    long long started = millis();
    if (started < 0) { perror("clock"); close(null_fd); fclose(output); return 2; }
    pid_t child = fork();
    if (child < 0) { perror("fork"); close(null_fd); fclose(output); return 2; }
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        struct rlimit limit = {2000000, 2000000};
        if (setpgid(0, 0) < 0 || chdir(argv[1]) < 0 ||
            setrlimit(RLIMIT_FSIZE, &limit) < 0 ||
            dup2(null_fd, STDIN_FILENO) < 0 ||
            dup2(output_fd, STDOUT_FILENO) < 0 || dup2(output_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(null_fd);
        if (output_fd > STDERR_FILENO) close(output_fd);
        execl("/bin/bash", "bash", "--noprofile", "--norc", "-c", argv[3], (char *)NULL);
        _exit(127);
    }
    close(null_fd);
    /* Both processes establish the group, closing the post-fork signal race. */
    if (setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        fclose(output);
        fprintf(stderr, "cannot establish child process group\n");
        return 2;
    }
    int status = 0, reaped = 0, timed_out = 0, failed = 0;
    while (!stopped) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) { reaped = 1; break; }
        if (result < 0 && errno != EINTR) { failed = 1; break; }
        long long now = millis();
        if (now < 0) { failed = 1; break; }
        if (now - started >= timeout) { timed_out = 1; break; }
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    /* Kill leftover jobs too, even after a successful shell exit. */
    kill(-child, SIGKILL);
    if (!reaped) {
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        if (result != child) failed = 1;
    }
    if (stopped) { fclose(output); return 128 + stopped; }
    if (failed) { fclose(output); fprintf(stderr, "cannot wait for command\n"); return 2; }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    unsigned char buffer[16001];
    rewind(output);
    size_t count = fread(buffer, 1, sizeof buffer, output);
    if (ferror(output)) { fclose(output); return 2; }
    fclose(output);
    printf("{\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s}\n", code,
           timed_out ? "true" : "false", count > 16000 ? "true" : "false");
    if (count > 16000) count = 16000;
    if (fwrite(buffer, 1, count, stdout) != count) return 2;
    return 0;
}
