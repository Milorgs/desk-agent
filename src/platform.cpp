#include "desk.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "ui.hpp"
#include <cctype>
#include <cerrno>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <cstdarg>
#include <unordered_map>
#else
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif
int terminal_rows(){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info))return 0;
    return info.srWindow.Bottom-info.srWindow.Top+1;
#else
    struct winsize size{};
    return ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0?size.ws_row:0;
#endif
}
int terminal_columns(){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info))return 0;
    return info.srWindow.Right-info.srWindow.Left+1;
#else
    struct winsize size{};
    return ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0?size.ws_col:0;
#endif
}
bool terminal_supports_ansi(){
#ifdef _WIN32
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode=0;
    if(!GetConsoleMode(output,&mode))return false;
    constexpr DWORD vt=ENABLE_PROCESSED_OUTPUT|ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return (mode&vt)==vt||SetConsoleMode(output,mode|vt);
#else
    const char *term=env_value("TERM");
    return isatty(STDOUT_FILENO)&&term&&*term&&strcmp(term,"dumb")!=0;
#endif
}
bool terminal_supports_color(){
    const char *no_color=env_value("NO_COLOR");
    return (!no_color||!*no_color)&&terminal_supports_ansi();
}
#ifdef _WIN32
std::wstring wide(std::string_view value){
    if(value.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(!size)throw std::runtime_error("Invalid UTF-8");
    std::wstring result(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size);return result;
}
std::string utf8(std::wstring_view value){
    if(value.empty())return {};
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!size)throw std::runtime_error("Invalid UTF-16");
    std::string result(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);return result;
}
const char *env_value(const char *name){
    static thread_local std::unordered_map<std::string,std::string> values;
    auto key=wide(name);DWORD size=GetEnvironmentVariableW(key.c_str(),nullptr,0);if(!size)return nullptr;
    std::wstring value(size,L'\0');DWORD used=GetEnvironmentVariableW(key.c_str(),value.data(),size);if(!used||used>=size)return nullptr;
    value.resize(used);values[name]=utf8(value);return values[name].c_str();
}
int asprintf(char **result,const char *format,...){
    *result=nullptr;va_list args;va_start(args,format);va_list count;va_copy(count,args);int size=_vscprintf(format,count);va_end(count);
    if(size<0){va_end(args);return -1;}*result=static_cast<char *>(malloc(static_cast<size_t>(size)+1));
    if(!*result){va_end(args);return -1;}vsnprintf(*result,static_cast<size_t>(size)+1,format,args);va_end(args);return size;
}
#endif
std::string config_base(){
    const char *value=env_value("XDG_CONFIG_HOME");if(value&&*value)return value;
#ifdef _WIN32
    value=env_value("LOCALAPPDATA");if(value&&*value)return value;
    value=env_value("USERPROFILE");return std::string(value?value:".")+"/AppData/Local";
#else
    value=env_value("HOME");return std::string(value?value:".")+"/.config";
#endif
}
std::string state_base(){
    const char *value=env_value("XDG_STATE_HOME");if(value&&*value)return value;
#ifdef _WIN32
    value=env_value("LOCALAPPDATA");if(value&&*value)return value;
    value=env_value("USERPROFILE");return std::string(value?value:".")+"/AppData/Local";
#else
    value=env_value("HOME");return std::string(value?value:".")+"/.local/state";
#endif
}
char *canonical_directory(const char *path,char *out){
    std::error_code ec;auto resolved=fs::canonical(fs_path(path),ec);if(ec||!fs::is_directory(resolved,ec))return nullptr;
    auto text=path_text(resolved);if(text.size()>=PATH_MAX)return nullptr;memcpy(out,text.c_str(),text.size()+1);return out;
}
bool path_inside(const fs::path &root,const fs::path &path){
#ifdef _WIN32
    // generic_u8string() uses '/', while canonical Windows paths can use '\\'.
    // Normalize separators before comparing individual components.
    auto preferred_root=root,preferred_path=path;
    preferred_root.make_preferred();preferred_path.make_preferred();
    auto a=preferred_root.begin(),b=preferred_path.begin();
    const auto a_end=preferred_root.end(),b_end=preferred_path.end();
#else
    auto a=root.begin(),b=path.begin();
    const auto a_end=root.end(),b_end=path.end();
#endif
    for(;a!=a_end;++a,++b){
        if(b==b_end)return false;
#ifdef _WIN32
        if(CompareStringOrdinal(a->native().c_str(),-1,b->native().c_str(),-1,TRUE)!=CSTR_EQUAL)return false;
#else
        if(*a!=*b)return false;
#endif
    }
    return true;
}
bool path_link(const fs::path &path){
#ifdef _WIN32
    DWORD attributes=GetFileAttributesW(path.c_str());return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;return fs::is_symlink(fs::symlink_status(path,ec));
#endif
}
#ifndef _WIN32
void platform_initialize(){}
const char *env_value(const char *name){return std::getenv(name);}
#endif

std::optional<int> terminal_choice(const std::vector<std::string> &items,int initial,int top,const std::function<void()> &on_idle,bool transient){
    if(items.empty())return 0;
    if(input_closed||interrupted==SIGTERM)return -1;
    if(!isatty(STDIN_FILENO)||!terminal_supports_ansi()||env_value("DESK_NO_ARROW"))return std::nullopt;
    interrupted=0;
    enum {idle=-1,end_input=-2,ignored=-3,up=256,down=257,home=258,end=259,page_up=260,page_down=261};
#ifdef _WIN32
    HANDLE console=GetStdHandle(STD_INPUT_HANDLE);
    DWORD original=0;
    if(!GetConsoleMode(console,&original))return std::nullopt;
    auto read_key=[&]() -> int {
        auto ready=WaitForSingleObject(console,50);
        if(ready==WAIT_TIMEOUT)return idle;
        if(ready!=WAIT_OBJECT_0)return end_input;
        INPUT_RECORD event{};
        DWORD count=0;
        if(!ReadConsoleInputW(console,&event,1,&count)||!count)return end_input;
        if(event.EventType!=KEY_EVENT||!event.Event.KeyEvent.bKeyDown)return ignored;
        const auto &key=event.Event.KeyEvent;
        if(key.wVirtualKeyCode==VK_UP)return up;
        if(key.wVirtualKeyCode==VK_DOWN)return down;
        if(key.wVirtualKeyCode==VK_HOME)return home;
        if(key.wVirtualKeyCode==VK_END)return end;
        if(key.wVirtualKeyCode==VK_PRIOR)return page_up;
        if(key.wVirtualKeyCode==VK_NEXT)return page_down;
        if(key.wVirtualKeyCode=='D'&&(key.dwControlKeyState&(LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED)))return 4;
        return key.uChar.UnicodeChar;
    };
#else
    termios original{};
    if(tcgetattr(STDIN_FILENO,&original))return std::nullopt;
    termios mode=original;
    mode.c_lflag&=~static_cast<tcflag_t>(ICANON|ECHO);
    mode.c_cc[VMIN]=1;
    mode.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&mode))return std::nullopt;
    struct RestoreTerminal {
        termios mode;
        ~RestoreTerminal(){tcsetattr(STDIN_FILENO,TCSANOW,&mode);}
    } restore{original};
    auto read_key=[]() -> int {
        pollfd ready{STDIN_FILENO,POLLIN,0};
        int result=poll(&ready,1,50);
        if(result<0)return errno==EINTR?ignored:end_input;
        if(!result)return idle;
        unsigned char key=0;
        return read(STDIN_FILENO,&key,1)==1?static_cast<int>(key):end_input;
    };
#endif
    size_t selected=initial>0&&static_cast<size_t>(initial)<=items.size()?static_cast<size_t>(initial-1):items.size()-1;
    int rows=terminal_rows(),columns=terminal_columns();
    auto capacity=[&](int height){return std::min(items.size(),static_cast<size_t>(std::max(1,height-top)));};
    size_t visible=capacity(rows);
    std::vector<std::string> labels=items;
    for(auto &label:labels)for(char &byte:label){
        if(static_cast<unsigned char>(byte)<32||byte==127)byte=' ';
    }
    size_t first=0;
    std::string typed;
    const bool color=terminal_supports_color();
    auto draw=[&]{
        if(selected<first)first=selected;
        if(selected>=first+visible)first=selected-visible+1;
        fputs("\033[?7l",stdout);
        for(size_t i=first;i<first+visible;i++){
            const bool focused=i==selected;
            // The focused row gets the accent colour and a bold label. Unselected
            // rows keep full brightness: dim text disappears on translucent
            // backgrounds, which is a common Konsole setup.
            std::string label=std::to_string((i+1)%items.size())+". "+labels[i];
            if(focused&&color)label=ui_paint(Tone::Accent,label,true);
            printf("\r\033[2K%s%s\n",focused?"❯ ":"  ",label.c_str());
        }
        fputs("\033[?7h",stdout);
        fflush(stdout);
    };
    draw();
    auto finish=[&](int choice){
        if(transient){printf("\033[%zuA\r\033[J",visible);fflush(stdout);}
        return std::optional<int>{choice};
    };
    std::string escape;
    double escape_started=0;
    while(!interrupted){
        if(on_idle)on_idle();
        int height=terminal_rows(),width=terminal_columns();
        if(height!=rows||width!=columns){
            size_t rewind=std::min(visible,capacity(height));
            printf("\033[%zuA\r\033[J",rewind);
            rows=height;columns=width;visible=capacity(rows);
            first=std::min(first,items.size()-visible);
            draw();
        }
        int key=read_key();
        if(key==end_input)break;
        if(key==idle){
            if(!escape.empty()&&monotime()-escape_started>=0.2){
                if(escape=="\x1b"&&transient)return finish(0);
                escape.clear();
            }
            continue;
        }
        if(key==ignored)continue;
        if(key=='\r'||key=='\n'){
            if(typed.empty())return finish(static_cast<int>((selected+1)%items.size()));
            auto value=strtoul(typed.c_str(),nullptr,10);
            if(value<items.size())return finish(static_cast<int>(value));
            typed.clear();
            continue;
        }
        if(key==4){input_closed=true;break;}
        if(key==27){escape="\x1b";escape_started=monotime();continue;}
        if(!escape.empty()){
            escape+=static_cast<char>(key);
            if(escape.size()==2&&(key=='['||key=='O'))continue;
            if(escape.size()>2&&((key>='0'&&key<='9')||key==';')&&escape.size()<16)continue;
            if(escape=="\x1b[A"||escape=="\x1bOA")key=up;
            else if(escape=="\x1b[B"||escape=="\x1bOB")key=down;
            else if(escape=="\x1b[H"||escape=="\x1bOH"||escape=="\x1b[1~")key=home;
            else if(escape=="\x1b[F"||escape=="\x1bOF"||escape=="\x1b[4~")key=end;
            else if(escape=="\x1b[5~")key=page_up;
            else if(escape=="\x1b[6~")key=page_down;
            else key=ignored;
            escape.clear();
        }
        const size_t previous=selected;
        if(key==up||key==down){
            selected=(selected+(key==up?items.size()-1:1))%items.size();
            typed.clear();
        }else if(key==home||key==end){
            selected=key==home?0:items.size()-1;
            typed.clear();
        }else if(key==page_up||key==page_down){
            selected=key==page_up?(selected>visible?selected-visible:0):std::min(selected+visible,items.size()-1);
            typed.clear();
        }else if(key==127||key==8){
            if(!typed.empty())typed.pop_back();
        }else if(key>='0'&&key<='9'){
            if(typed.size()<20)typed.push_back(static_cast<char>(key));
        }
        if(!typed.empty()){
            auto value=strtoul(typed.c_str(),nullptr,10);
            if(value<items.size())selected=value?value-1:items.size()-1;
        }
        if(previous!=selected){printf("\033[%zuA",visible);draw();}
    }
    return finish(-1);
}
