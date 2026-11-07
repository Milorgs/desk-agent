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


// Read console events so chat menus and task updates do not wait for Enter.
void terminal_message(const std::string &text){
    fwrite(text.data(),1,text.size(),stdout);
    fflush(stdout);
}
char *editable_input(const char *prompt,const char *fallback,bool secret,const std::function<void()> &idle,ChatDraft *draft){
    HANDLE console=GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode=0;
    if(secret||!GetConsoleMode(console,&mode)||!terminal_supports_ansi())return input(prompt,fallback,secret);
    if(input_closed)return nullptr;
    std::wstring text=draft?wide(draft->text):std::wstring{};
    size_t point=draft?wide(draft->text.substr(0,std::min(draft->text.size(),static_cast<size_t>(std::max(0,draft->cursor))))).size():text.size();
    wchar_t pending_surrogate=0;
    if(draft){draft->paused=false;}
    auto previous=[&](size_t offset){
        if(offset)offset--;
        if(offset&&text[offset]>=0xdc00&&text[offset]<=0xdfff)offset--;
        return offset;
    };
    auto next=[&](size_t offset){
        if(offset<text.size()){
            if(text[offset]>=0xd800&&text[offset]<=0xdbff)offset++;
            offset++;
        }
        return std::min(offset,text.size());
    };
    auto cell_width=[](std::wstring_view value){
        int cells=0;
        for(size_t i=0;i<value.size();i++){
            wchar_t ch=value[i];
            if(ch>=0xd800&&ch<=0xdbff&&i+1<value.size()){cells+=2;i++;continue;}
            WORD type=0;
            GetStringTypeW(CT_CTYPE3,&ch,1,&type);
            if(type&(C3_NONSPACING|C3_DIACRITIC))continue;
            cells+=(type&C3_FULLWIDTH)?2:1;
        }
        return cells;
    };
    auto redraw=[&]{
        const int available=std::max(1,terminal_columns()-cell_width(wide(prompt))-1);
        size_t first=point;
        int cells=0;
        while(first){
            size_t begin=previous(first);
            int width=cell_width(std::wstring_view(text).substr(begin,first-begin));
            if(cells+width>=available)break;
            cells+=width;first=begin;
        }
        size_t last=point;
        while(last<text.size()){
            size_t end=next(last);
            int width=cell_width(std::wstring_view(text).substr(last,end-last));
            if(cells+width>available)break;
            cells+=width;last=end;
        }
        printf("\r\033[2K\033[?7l%s",prompt);
        auto before=utf8(std::wstring_view(text).substr(first,point-first));
        auto after=utf8(std::wstring_view(text).substr(point,last-point));
        fputs(before.c_str(),stdout);
        fputs("\0337",stdout);
        fputs(after.c_str(),stdout);
        fputs("\0338\033[?7h",stdout);
        fflush(stdout);
    };
    redraw();
    while(!interrupted){
        if(idle)idle();
        if(WaitForSingleObject(console,50)!=WAIT_OBJECT_0)continue;
        INPUT_RECORD record{};
        DWORD count=0;
        if(!ReadConsoleInputW(console,&record,1,&count)||!count)break;
        if(record.EventType==WINDOW_BUFFER_SIZE_EVENT){redraw();continue;}
        if(record.EventType!=KEY_EVENT||!record.Event.KeyEvent.bKeyDown)continue;
        const auto &key=record.Event.KeyEvent;
        bool ctrl=(key.dwControlKeyState&(LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))!=0;
        if(ctrl&&key.wVirtualKeyCode=='D'){input_closed=true;break;}
        if(draft&&(key.wVirtualKeyCode==VK_F2||key.wVirtualKeyCode==VK_F3)){
            draft->text=utf8(text);
            draft->cursor=static_cast<int>(utf8(std::wstring_view(text).substr(0,point)).size());
            fputs("\r\033[2K",stdout);
            return _strdup(key.wVirtualKeyCode==VK_F2?"/model":"/reasoning");
        }
        if(key.wVirtualKeyCode==VK_RETURN){
            putchar('\n');
            auto line=utf8(text);
            if(draft){draft->text.clear();draft->cursor=0;}
            return _strdup(line.empty()&&fallback?fallback:line.c_str());
        }
        if(key.wVirtualKeyCode==VK_LEFT)point=previous(point);
        else if(key.wVirtualKeyCode==VK_RIGHT)point=next(point);
        else if(key.wVirtualKeyCode==VK_HOME)point=0;
        else if(key.wVirtualKeyCode==VK_END)point=text.size();
        else if(key.wVirtualKeyCode==VK_BACK&&point){size_t begin=previous(point);text.erase(begin,point-begin);point=begin;}
        else if(key.wVirtualKeyCode==VK_DELETE&&point<text.size())text.erase(point,next(point)-point);
        else if(!ctrl&&key.uChar.UnicodeChar>=32&&text.size()<65536){
            if(draft&&text.empty()&&key.uChar.UnicodeChar=='/'){
                fputs("\r\033[2K",stdout);
                return _strdup("/");
            }
            wchar_t value=key.uChar.UnicodeChar;
            if(value>=0xd800&&value<=0xdbff){pending_surrogate=value;continue;}
            std::wstring insertion;
            if(value>=0xdc00&&value<=0xdfff){
                if(pending_surrogate)insertion+=pending_surrogate;
                else value=0xfffd;
            }else if(pending_surrogate)insertion+=static_cast<wchar_t>(0xfffd);
            pending_surrogate=0;
            insertion+=value;
            for(WORD i=0;i<key.wRepeatCount&&text.size()+insertion.size()<=65536;i++){
                text.insert(point,insertion);
                point+=insertion.size();
            }
        }
        redraw();
    }
    putchar('\n');
    return nullptr;
}
