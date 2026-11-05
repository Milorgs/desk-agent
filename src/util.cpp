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
char *input(const char *prompt,const char *fallback,bool secret){
    if(input_closed)return nullptr;
    printf("%s",prompt);
    fflush(stdout);
    struct termios old;
    bool changed=false;
    if(secret&&isatty(STDIN_FILENO)&&!tcgetattr(STDIN_FILENO,&old)){
        struct termios t=old;
        t.c_lflag&=~ECHO;
        if(!tcsetattr(STDIN_FILENO,TCSANOW,&t))changed=true;
    }
    char *line=NULL;
    size_t capacity=0;
    ssize_t n=getline(&line,&capacity,stdin);
    if(changed){
        tcsetattr(STDIN_FILENO,TCSANOW,&old);
        putchar('\n');
    }
    if(n<0){
        if(feof(stdin)&&!interrupted)input_closed=true;
        free(line);
        clearerr(stdin);
        return NULL;
    }
    if(n>65536){
        free(line);
        return NULL;
    }
    while(n>0&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
    if(!n&&fallback){
        free(line);
        return strdup(fallback);
    }
    return line;
}

// Detect a canonical terminal EOF without consuming queued request text.
void check_exit(const App &app){
    if(app.task_events>=0||input_closed||!isatty(STDIN_FILENO))return;
    struct termios mode{};if(tcgetattr(STDIN_FILENO,&mode)||!(mode.c_lflag&ICANON))return;
    struct pollfd ready{STDIN_FILENO,POLLIN,0};int bytes=0;
    if(poll(&ready,1,0)>0&&(ready.revents&(POLLIN|POLLHUP))&&!ioctl(STDIN_FILENO,FIONREAD,&bytes)&&bytes==0){input_closed=true;interrupted=SIGINT;}
}
