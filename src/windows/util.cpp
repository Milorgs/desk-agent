#include "win.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
namespace {
HANDLE main_thread=nullptr;
BOOL WINAPI control(DWORD event){
    if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT&&event!=CTRL_CLOSE_EVENT&&event!=CTRL_LOGOFF_EVENT&&event!=CTRL_SHUTDOWN_EVENT)return FALSE;
    interrupted=event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT?SIGINT:SIGTERM;
    if(main_thread)CancelSynchronousIo(main_thread);return TRUE;
}
void file_errno(){DWORD code=GetLastError();errno=code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND?ENOENT:EACCES;}
}
void platform_initialize(){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
    if(!main_thread){DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&main_thread,0,FALSE,DUPLICATE_SAME_ACCESS);SetConsoleCtrlHandler(control,TRUE);}
}
char *read_bytes(const char *path,size_t limit,size_t *length){
    auto target=fs_path(path);WinHandle file(CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file){file_errno();return nullptr;}
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.get(),&info)||!GetFileSizeEx(file.get(),&size)||GetFileType(file.get())!=FILE_TYPE_DISK||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||size.QuadPart<0||static_cast<unsigned long long>(size.QuadPart)>limit){errno=EFBIG;return nullptr;}
    char *data=static_cast<char *>(malloc(limit+1));if(!data)return nullptr;size_t used=0;
    while(used<limit){DWORD count=0;if(!ReadFile(file.get(),data+used,static_cast<DWORD>((std::min)(limit-used,static_cast<size_t>(65536))),&count,nullptr)){free(data);file_errno();return nullptr;}if(!count)break;used+=count;}
    if(used==limit){char byte;DWORD count=0;if(!ReadFile(file.get(),&byte,1,&count,nullptr)||count){free(data);errno=EFBIG;return nullptr;}}
    data[used]=0;if(length)*length=used;return data;
}
int mkdirs(const char *path,unsigned int mode){
    fs::path target=fs_path(path),current;PrivateAccess access;
    if(mode==0700&&!access.descriptor)return -1;
    for(const auto &part:target){
        current/=part;if(current==current.root_path())continue;
        DWORD attributes=GetFileAttributesW(current.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            if(!CreateDirectoryW(current.c_str(),mode==0700?&access.attributes:nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){file_errno();return -1;}
            attributes=GetFileAttributesW(current.c_str());
        }
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){errno=EACCES;return -1;}
    }
    if(mode==0700&&!access.apply(target)){file_errno();return -1;}
    return 0;
}
int atomic_file(const char *path,const void *data,size_t length,unsigned int mode){
    fs::path target=fs_path(path);char id[33];id_string(id);fs::path temporary=target;temporary+=L".tmp-"+wide(id);
    PrivateAccess access;if(mode==0600&&!access.descriptor)return -1;
    WinHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,mode==0600?&access.attributes:nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file){file_errno();return -1;}
    bool ok=win_write(file.get(),std::string_view(static_cast<const char *>(data),length))&&FlushFileBuffers(file.get());file.reset();
    DWORD attributes=GetFileAttributesW(target.c_str());
    if(ok&&attributes!=INVALID_FILE_ATTRIBUTES){
        if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))ok=false;
        else if(mode==0600&&!access.apply(target))ok=false;
        else ok=ReplaceFileW(target.c_str(),temporary.c_str(),nullptr,0,nullptr,nullptr)!=FALSE;
    }else if(ok)ok=MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok){file_errno();DeleteFileW(temporary.c_str());return -1;}return 0;
}
char *input(const char *prompt,const char *fallback,bool secret){
    if(input_closed)return nullptr;
    fputs(prompt,stdout);fflush(stdout);HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);DWORD mode=0;
    std::string line;bool console=GetConsoleMode(handle,&mode)!=FALSE,ok=true;
    if(console){
        if(secret&&!SetConsoleMode(handle,mode&~ENABLE_ECHO_INPUT))return nullptr;
        std::wstring text;wchar_t buffer[1024];DWORD count=0;
        while(!interrupted){
            if(!ReadConsoleW(handle,buffer,1024,&count,nullptr)||!count){ok=false;break;}
            text.append(buffer,count);if(text.size()>65536){ok=false;break;}if(text.find(L'\n')!=std::wstring::npos)break;
        }
        if(secret){SetConsoleMode(handle,mode);putchar('\n');}
        if(!text.empty()&&(text.front()==4||text.front()==26))ok=false;
        if(ok&&!interrupted)line=utf8(text);
    }else{
        bool pipe=GetFileType(handle)==FILE_TYPE_PIPE;
        while(!interrupted){
            if(pipe){DWORD available=0;if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)){ok=false;break;}if(!available){Sleep(20);continue;}}
            char byte;DWORD count=0;if(!ReadFile(handle,&byte,1,&count,nullptr)||!count){ok=false;break;}if(byte=='\n')break;line+=byte;if(line.size()>65536){ok=false;break;}
        }
    }
    if(!ok&&!interrupted)input_closed=true;
    if(!ok||interrupted||line.size()>65536)return nullptr;
    while(!line.empty()&&(line.back()=='\r'||line.back()=='\n'))line.pop_back();
    return _strdup(line.empty()&&fallback?fallback:line.c_str());
}

void check_exit(const App &app){(void)app;}
