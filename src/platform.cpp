#include "desk.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#include <cstdarg>
#include <unordered_map>
#else
#include <sys/ioctl.h>
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
