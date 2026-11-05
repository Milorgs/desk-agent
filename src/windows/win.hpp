#ifndef DESK_WIN_HPP
#define DESK_WIN_HPP
#include "desk.hpp"
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <thread>
#include <mutex>
#include <algorithm>
#include <stdexcept>
struct WinHandle {
    HANDLE value=nullptr;
    WinHandle()=default;
    explicit WinHandle(HANDLE handle):value(handle){}
    WinHandle(const WinHandle &)=delete;
    WinHandle &operator=(const WinHandle &)=delete;
    WinHandle(WinHandle &&other) noexcept:value(other.release()){}
    WinHandle &operator=(WinHandle &&other) noexcept {reset(other.release());return *this;}
    ~WinHandle(){reset();}
    explicit operator bool() const {return value&&value!=INVALID_HANDLE_VALUE;}
    HANDLE get() const {return value;}
    HANDLE release(){HANDLE result=value;value=nullptr;return result;}
    void reset(HANDLE handle=nullptr){if(*this)CloseHandle(value);value=handle;}
};
struct PrivateAccess {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    PrivateAccess(){
        HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return;WinHandle token(raw);
        DWORD bytes=0;GetTokenInformation(token.get(),TokenUser,nullptr,0,&bytes);std::vector<unsigned char> data(bytes);
        if(!GetTokenInformation(token.get(),TokenUser,data.data(),bytes,&bytes))return;
        LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,&sid))return;
        std::wstring sddl=L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);
        if(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))attributes.lpSecurityDescriptor=descriptor;
    }
    ~PrivateAccess(){if(descriptor)LocalFree(descriptor);}
    bool apply(const fs::path &path){return descriptor&&SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor);}
};
inline bool win_write(HANDLE handle,std::string_view text){
    while(!text.empty()){
        DWORD written=0;DWORD count=static_cast<DWORD>((std::min)(text.size(),static_cast<size_t>(32768)));
        if(!WriteFile(handle,text.data(),count,&written,nullptr)||!written)return false;text.remove_prefix(written);
    }
    return true;
}
inline bool win_pipe(WinHandle &read,WinHandle &write){
    SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};HANDLE r=nullptr,w=nullptr;
    if(!CreatePipe(&r,&w,&attr,65536))return false;read.reset(r);write.reset(w);return true;
}
inline WinHandle kill_job(){
    WinHandle job(CreateJobObjectW(nullptr,nullptr));if(!job)return {};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&info,sizeof info))return {};return job;
}
std::vector<wchar_t> command_environment();
bool win_spawn(const std::wstring &executable,std::wstring command,const std::wstring &directory,HANDLE input,HANDLE output,const std::vector<HANDLE> &inherit,HANDLE job,WinHandle &process);
inline std::wstring quote_arg(std::wstring_view arg){
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:arg){if(c==L'\\'){slashes++;continue;}if(c==L'\"')result.append(slashes*2+1,L'\\');else result.append(slashes,L'\\');slashes=0;result+=c;}
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
std::string safe_output(std::string_view bytes);
#endif
