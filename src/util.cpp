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
// Consume the canonical EOF marker before switching the terminal to raw input.
// Queued request text stays untouched.
bool terminal_eof(){
    if(!isatty(STDIN_FILENO))return false;
    struct termios mode{};if(tcgetattr(STDIN_FILENO,&mode)||!(mode.c_lflag&ICANON))return false;
    struct pollfd ready{STDIN_FILENO,POLLIN,0};int bytes=0;
    if(poll(&ready,1,0)<=0||!(ready.revents&(POLLIN|POLLHUP))||ioctl(STDIN_FILENO,FIONREAD,&bytes)||bytes)return false;
    char marker;
    return read(STDIN_FILENO,&marker,1)==0;
}
struct EditorState {char *line=nullptr;bool done=false,installed=false;std::string action;};
EditorState *editor=nullptr;
bool activity_visible=false;
std::string last_activity;
void accepted_line(char *line){
    activity_visible=false;
    editor->line=line;editor->done=true;
    rl_callback_handler_remove();editor->installed=false;
}
int choose_chat_model(int,int){
    if(editor)editor->action="/model";
    return 0;
}
int choose_chat_reasoning(int,int){
    if(editor)editor->action="/reasoning";
    return 0;
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
    ~EditorSession(){
        for(const char *sequence:{"\033OQ","\033OR","\033[12~","\033[13~"})rl_bind_keyseq(sequence,rl_insert);
        if(state.installed)rl_callback_handler_remove();
        editor=nullptr;
        activity_visible=false;
    }
};
}
void terminal_message(const std::string &text){
    if(text.empty())return;
    activity_visible=false;
    if(!editor||!editor->installed){fwrite(text.data(),1,text.size(),stdout);fflush(stdout);return;}
    std::string line=rl_line_buffer;
    int point=rl_point;
    rl_save_prompt();rl_replace_line("",0);rl_redisplay();
    fputs("\r\033[2K",stdout);
    fwrite(text.data(),1,text.size(),stdout);
    if(text.back()!='\n')putchar('\n');
    rl_restore_prompt();rl_replace_line(line.c_str(),0);rl_point=point;
    rl_on_new_line();rl_redisplay();fflush(stdout);
}
void terminal_activity(const std::string &text){
    if(!editor||!editor->installed){
        activity_visible=false;
        return;
    }
    if(!activity_visible){
        if(text.empty())return;
        terminal_message(text+"\n");
        activity_visible=true;
        last_activity=text;
        return;
    }
    if(text==last_activity)return;
    last_activity=text;
    fputs("\0337\033[1A\r\033[2K",stdout);
    fwrite(text.data(),1,text.size(),stdout);
    fputs("\0338",stdout);
    fflush(stdout);
    if(text.empty())activity_visible=false;
}
char *editable_input(const char *prompt,const char *fallback,bool secret,const std::function<void()> &idle,ChatDraft *draft){
    if(input_closed)return nullptr;
    if(!secret&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)){
        if(terminal_eof()){input_closed=true;putchar('\n');return nullptr;}
        EditorSession session(prompt);
        if(draft){
            draft->paused=false;
            rl_bind_keyseq("\033OQ",choose_chat_model);
            rl_bind_keyseq("\033OR",choose_chat_reasoning);
            rl_bind_keyseq("\033[12~",choose_chat_model);
            rl_bind_keyseq("\033[13~",choose_chat_reasoning);
            if(!draft->text.empty()){
                rl_replace_line(draft->text.c_str(),0);
                rl_point=draft->cursor;
                rl_redisplay();
            }
        }
        struct winsize previous{};ioctl(STDOUT_FILENO,TIOCGWINSZ,&previous);
        while(!session.state.done&&!interrupted){
            if(idle)idle();
            if(draft&&draft->approval_pending&&draft->approval_pending()){
                draft->text.assign(rl_line_buffer,static_cast<size_t>(rl_end));
                draft->cursor=rl_point;
                draft->paused=true;
                fputs("\r\033[2K",stdout);
                return nullptr;
            }
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
            if(draft&&!session.state.done&&!session.state.action.empty()){
                draft->text.assign(rl_line_buffer,static_cast<size_t>(rl_end));
                draft->cursor=rl_point;
                fputs("\r\033[2K",stdout);
                return strdup(session.state.action.c_str());
            }
            if(draft&&!session.state.done&&rl_last_func==rl_insert&&rl_end==1&&rl_line_buffer[0]=='/'){
                draft->text.clear();draft->cursor=0;
                fputs("\r\033[2K",stdout);
                return strdup("/");
            }
            if(!session.state.done&&rl_end>65536){
                rl_replace_line("",0);rl_crlf();puts(tr("输入超过 64 KB，请缩短后重试。"));rl_on_new_line();rl_redisplay();
            }
        }
        if(draft){draft->text.clear();draft->cursor=0;}
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

void check_exit(const App &app){
    if(app.task_events<0&&!input_closed&&terminal_eof()){input_closed=true;interrupted=SIGINT;}
}
