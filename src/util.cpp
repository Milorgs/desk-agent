#include "desk.hpp"
#include "tasks.hpp"
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <readline/readline.h>
char *read_bytes(const char *path,size_t limit,size_t *length){
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(size_t)st.st_size>limit){
        close(fd);
        errno=EFBIG;
        return NULL;
    }
    char *buffer=static_cast<char *>(malloc(limit+1));
    if(!buffer){
        close(fd);
        return NULL;
    }
    size_t n=0;
    while(n<limit){
        ssize_t r=read(fd,buffer+n,limit-n);
        if(r<0&&errno==EINTR)continue;
        if(r<0){
            free(buffer);
            close(fd);
            return NULL;
        }
        if(!r)break;
        n+=(size_t)r;
    }
    if(n==limit){
        char extra;
        ssize_t extra_count;
        do{extra_count=read(fd,&extra,1);}while(extra_count<0&&errno==EINTR);
        if(extra_count!=0){free(buffer);close(fd);errno=EFBIG;return NULL;}
    }
    close(fd);
    buffer[n]=0;
    if(length)*length=n;
    return buffer;
}
int mkdirs(const char *path,unsigned int mode){
    char temp[PATH_MAX];
    if(strlen(path)>=sizeof temp)return -1;
    strcpy(temp,path);
    for(char *p=temp+1;;p++){
        if(*p=='/'||!*p){
            char c=*p;
            *p=0;
            struct stat st;
            if(lstat(temp,&st)){
                if(errno!=ENOENT||mkdir(temp,mode))return -1;
            }
            else if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -1;
            *p=c;
            if(!c)break;
        }
    }
    return 0;
}
int atomic_file(const char *path,const void *data,size_t len,unsigned int mode){
    char tmp[PATH_MAX];
    if(snprintf(tmp,sizeof tmp,"%s.tmp-XXXXXX",path)>=(int)sizeof tmp)return -1;
    int fd=mkstemp(tmp);
    if(fd<0)return -1;
    int rc=fchmod(fd,mode);
    size_t n=0;
    while(!rc&&n<len){
        ssize_t w=write(fd,(const char *)data+n,len-n);
        if(w<0&&errno==EINTR)continue;
        if(w<=0){
            rc=-1;
            break;
        }
        n+=(size_t)w;
    }
    if(!rc)rc=fsync(fd);
    if(close(fd)&&!rc)rc=-1;
    if(!rc)rc=rename(tmp,path);
    if(rc)unlink(tmp);
    return rc;
}
namespace {
struct EditorState {char *line=nullptr;bool done=false,installed=false;};
EditorState *editor=nullptr;
void accepted_line(char *line){
    editor->line=line;editor->done=true;
    rl_callback_handler_remove();editor->installed=false;
}
struct EditorSession {
    EditorState state;
    explicit EditorSession(const char *prompt){
        editor=&state;
        rl_readline_name="desk";
        rl_catch_signals=0;rl_catch_sigwinch=0;
        rl_callback_handler_install(prompt,accepted_line);state.installed=true;
        // Keep editing predictable and never complete or save private input.
        rl_bind_key('\t',rl_insert);
        rl_variable_bind("horizontal-scroll-mode","on");
    }
    ~EditorSession(){if(state.installed)rl_callback_handler_remove();editor=nullptr;}
};
}
char *editable_input(const char *prompt,const char *fallback,bool secret,const std::function<void()> &idle){
    if(input_closed)return nullptr;
    if(!secret&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)){
        EditorSession session(prompt);
        struct winsize previous{};ioctl(STDOUT_FILENO,TIOCGWINSZ,&previous);
        while(!session.state.done&&!interrupted){
            if(idle)idle();
            struct pollfd ready{STDIN_FILENO,POLLIN,0};int rc=poll(&ready,1,50);
            if(interrupted)break;
            if(rc<0&&errno!=EINTR)break;
            struct winsize current{};
            if(!ioctl(STDOUT_FILENO,TIOCGWINSZ,&current)&&(current.ws_col!=previous.ws_col||current.ws_row!=previous.ws_row)){
                previous=current;
                fputs("\r\033[2K",stdout);
                rl_on_new_line();
                rl_set_screen_size(current.ws_row,current.ws_col);
                rl_forced_update_display();
            }
            if(rc>0&&(ready.revents&(POLLIN|POLLHUP)))rl_callback_read_char();
            if(!session.state.done&&rl_end>65536){
                rl_replace_line("",0);rl_crlf();puts(tr("输入超过 64 KB，请缩短后重试。"));rl_on_new_line();rl_redisplay();
            }
        }
        if(interrupted){rl_replace_line("",0);rl_crlf();free(session.state.line);return nullptr;}
        if(!session.state.done)return nullptr;
        char *line=session.state.line;
        if(!line){input_closed=true;putchar('\n');return nullptr;}
        if(strlen(line)>65536){free(line);puts(tr("输入超过 64 KB，请缩短后重试。"));return strdup("");}
        if(!*line&&fallback){free(line);return strdup(fallback);}
        return line;
    }
    printf("%s",prompt);fflush(stdout);
    struct termios old{};bool changed=false;
    if(secret&&isatty(STDIN_FILENO)&&!tcgetattr(STDIN_FILENO,&old)){
        struct termios mode=old;mode.c_lflag&=~ECHO;
#ifdef IUTF8
        mode.c_iflag|=IUTF8;
#endif
        changed=!tcsetattr(STDIN_FILENO,TCSANOW,&mode);
    }
    char *line=nullptr;size_t capacity=0;ssize_t n=getline(&line,&capacity,stdin);
    if(changed){tcsetattr(STDIN_FILENO,TCSANOW,&old);putchar('\n');}
    if(n<0){if(feof(stdin)&&!interrupted)input_closed=true;free(line);clearerr(stdin);return nullptr;}
    if(n>65536){free(line);return nullptr;}
    while(n>0&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
    if(!n&&fallback){free(line);return strdup(fallback);}
    return line;
}
char *input(const char *prompt,const char *fallback,bool secret){return editable_input(prompt,fallback,secret,{});}

// Detect a canonical terminal EOF without consuming queued request text.
void check_exit(const App &app){
    if(app.task_events>=0||input_closed||!isatty(STDIN_FILENO))return;
    struct termios mode{};if(tcgetattr(STDIN_FILENO,&mode)||!(mode.c_lflag&ICANON))return;
    struct pollfd ready{STDIN_FILENO,POLLIN,0};int bytes=0;
    if(poll(&ready,1,0)>0&&(ready.revents&(POLLIN|POLLHUP))&&!ioctl(STDIN_FILENO,FIONREAD,&bytes)&&bytes==0){input_closed=true;interrupted=SIGINT;}
}
