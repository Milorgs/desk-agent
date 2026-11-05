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
#include <time.h>
volatile sig_atomic_t interrupted=0;
J *obj(void){
    return json_object_new_object();
}
J *arr(void){
    return json_object_new_array();
}
J *str(const char *s){
    return json_object_new_string(s?s:"");
}
J *get(J *j,const char *key){
    J *v=NULL;
    if(j&&json_object_is_type(j,json_type_object))json_object_object_get_ex(j,key,&v);
    return v;
}
const char *sget(J *j,const char *key){
    J *v=get(j,key);
    return v&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";
}
int iget(J *j,const char *key,int fallback){
    J *v=get(j,key);
    return v&&json_object_is_type(v,json_type_int)?json_object_get_int(v):fallback;
}
bool bget(J *j,const char *key){
    return json_object_get_boolean(get(j,key));
}
bool tool_failed(J *r){
    J *accepted=get(r,"accepted");
    return get(r,"error")||bget(r,"denied")||bget(r,"timed_out")||iget(r,"exit_code",0)!=0||(accepted&&!json_object_get_boolean(accepted));
}
bool valid_utf8(std::string_view text) noexcept {
    for(size_t i=0;i<text.size();){
        auto c=static_cast<unsigned char>(text[i]);
        size_t width=c<0x80?1:(c>=0xc2&&c<=0xdf)?2:(c>=0xe0&&c<=0xef)?3:(c>=0xf0&&c<=0xf4)?4:0;
        if(!width||width>text.size()-i)return false;
        for(size_t k=1;k<width;k++)if((static_cast<unsigned char>(text[i+k])&0xc0)!=0x80)return false;
        if(width>=3){
            auto next=static_cast<unsigned char>(text[i+1]);
            if((c==0xe0&&next<0xa0)||(c==0xed&&next>=0xa0)||(c==0xf0&&next<0x90)||(c==0xf4&&next>=0x90))return false;
        }
        i+=width;
    }
    return true;
}
void put(J *j,const char *key,J *v){
    json_object_object_add(j,key,v);
}
void add(J *j,J *v){
    json_object_array_add(j,v);
}
J *copy(J *j){
    J *v=NULL;
    if(j&&json_object_deep_copy(j,&v,NULL)!=0)return NULL;
    return v;
}
J *err(const char *m){
    J *v=obj();
    put(v,"error",str(m));
    return v;
}
void seterr(App *a,const char *m){
    snprintf(a->error,sizeof a->error,"%s",m);
}
double monotime(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return t.tv_sec+t.tv_nsec/1e9;
}
void id_string(char out[33]){
    unsigned char b[16];
    if(getrandom(b,16,0)!=16){
        perror("getrandom");
        exit(1);
    }
    for(int i=0;i<16;i++)sprintf(out+i*2,"%02x",b[i]);
}
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
int mkdirs(const char *path,mode_t mode){
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
int atomic_file(const char *path,const void *data,size_t len,mode_t mode){
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
int approve(App *a,const char *description){
    if(interrupted)return 0;
    printf("\n%s\n",description);
    if(a->yes)return 1;
    if(!isatty(STDIN_FILENO))return 0;
    task_event(*a,'a');
    char *v=input("允许？[y/N] ",NULL,false);
    task_event(*a,'r');
    int yes=v&&(!strcmp(v,"y")||!strcmp(v,"Y"));
    free(v);
    return yes;
}
void app_cleanup(App *a){
    desktop_clear(a);
    free(a->desktop_data);
    json_object_put(a->store);
    json_object_put(a->config);
    json_object_put(a->messages);
}
size_t alen(J *j){
    return j&&json_object_is_type(j,json_type_array)?json_object_array_length(j):0;
}
J *at(J *j,size_t index){
    return j&&json_object_is_type(j,json_type_array)?json_object_array_get_idx(j,index):NULL;
}
int path_suffix(char out[PATH_MAX],const char *base,const char *suffix){
    size_t n=strlen(base),m=strlen(suffix);
    if(n+m>=PATH_MAX){
        out[0]=0;
        return -1;
    }
    memcpy(out,base,n);
    memcpy(out+n,suffix,m+1);
    return 0;
}
