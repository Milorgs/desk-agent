#include "win.hpp"
#include <cwctype>
#include <cstring>
#include <openssl/evp.h>
std::vector<wchar_t> command_environment(){
    std::vector<wchar_t> result;LPWCH block=GetEnvironmentStringsW();if(!block)throw std::runtime_error("Environment unavailable");
    for(const wchar_t *entry=block;*entry;entry+=wcslen(entry)+1){
        const wchar_t *end=wcschr(entry+(*entry==L'='?1:0),L'=');if(!end)continue;
        std::wstring key(entry,end);for(auto &c:key)c=static_cast<wchar_t>(towupper(c));
        if(key.find(L"KEY")!=std::wstring::npos||key.find(L"TOKEN")!=std::wstring::npos||key.find(L"SECRET")!=std::wstring::npos||key.find(L"PASSWORD")!=std::wstring::npos)continue;
        result.insert(result.end(),entry,entry+wcslen(entry)+1);
    }
    FreeEnvironmentStringsW(block);result.push_back(0);if(result.size()==1)result.push_back(0);return result;
}
bool win_spawn(const std::wstring &executable,std::wstring command,const std::wstring &directory,HANDLE input,HANDLE output,const std::vector<HANDLE> &inherit,HANDLE job,WinHandle &process){
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
    auto *attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&bytes))return false;
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE *>(inherit.data()),inherit.size()*sizeof(HANDLE),nullptr,nullptr)){DeleteProcThreadAttributeList(attributes);return false;}
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof start;start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;start.StartupInfo.hStdInput=input;start.StartupInfo.hStdOutput=output;start.StartupInfo.hStdError=output;start.lpAttributeList=attributes;
    PROCESS_INFORMATION info{};auto environment=command_environment();
    BOOL created=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,environment.data(),directory.empty()?nullptr:directory.c_str(),&start.StartupInfo,&info);
    DeleteProcThreadAttributeList(attributes);if(!created)return false;
    WinHandle child(info.hProcess),thread(info.hThread);
    if(!AssignProcessToJobObject(job,child.get())||ResumeThread(thread.get())==static_cast<DWORD>(-1)){TerminateProcess(child.get(),1);WaitForSingleObject(child.get(),2000);return false;}
    process=std::move(child);return true;
}
std::string safe_output(std::string_view bytes){
    std::string result;
    for(size_t i=0;i<bytes.size();){
        auto c=static_cast<unsigned char>(bytes[i]);size_t size=c<0x80?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        if(size&&size<=bytes.size()-i&&valid_utf8(bytes.substr(i,size))){result.append(bytes.substr(i,size));i+=size;}
        else{result+="\xef\xbf\xbd";i++;}
    }
    return result;
}
J *run_command(App *app,const char *command,int timeout_ms){
    if(timeout_ms<1||timeout_ms>300000)return err(tr("命令超时设置无效。"));
    const char *root=env_value("SystemRoot");if(!root)return err("SystemRoot is unavailable.");
    std::wstring executable=wide(root)+L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::string script="[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); $OutputEncoding = [Console]::OutputEncoding; & {\n"+std::string(command)+"\n}; $desk_ok=$?; $desk_exit=$LASTEXITCODE; if($null -ne $desk_exit){exit $desk_exit}; if(!$desk_ok){exit 1}";
    std::wstring text=wide(script);std::string encoded(((text.size()*sizeof(wchar_t)+2)/3)*4,'\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),reinterpret_cast<const unsigned char *>(text.data()),static_cast<int>(text.size()*sizeof(wchar_t)));
    std::wstring arguments=quote_arg(executable)+L" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand "+wide(encoded);
    WinHandle read,write;if(!win_pipe(read,write))return err("Could not create command output pipe.");SetHandleInformation(read.get(),HANDLE_FLAG_INHERIT,0);
    SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};WinHandle null(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    WinHandle job=kill_job(),process;if(!null||!job||!win_spawn(executable,std::move(arguments),wide(app->root),null.get(),write.get(),{null.get(),write.get()},job.get(),process))return err(tr("无法启动命令。"));
    write.reset();std::string output;bool truncated=false,timed=false;double until=monotime()+timeout_ms/1000.0;DWORD code=1;
    auto drain=[&]{for(int i=0;i<16;i++){
        DWORD available=0;if(!PeekNamedPipe(read.get(),nullptr,0,nullptr,&available,nullptr)||!available)break;
        char bytes[4096];DWORD count=0;if(!ReadFile(read.get(),bytes,(std::min)(available,static_cast<DWORD>(sizeof bytes)),&count,nullptr)||!count)break;
        size_t keep=(std::min)(static_cast<size_t>(count),16000-output.size());output.append(bytes,keep);if(keep<count)truncated=true;
    }};
    for(;;){drain();if(WaitForSingleObject(process.get(),10)==WAIT_OBJECT_0){GetExitCodeProcess(process.get(),&code);break;}if(interrupted)break;if(monotime()>=until){timed=true;break;}}
    TerminateJobObject(job.get(),1);WaitForSingleObject(process.get(),2000);drain();
    if(interrupted)return err(tr("命令已取消。"));
    J *result=obj();put(result,"exit_code",json_object_new_int64(code));put(result,"timed_out",json_object_new_boolean(timed));put(result,"truncated",json_object_new_boolean(truncated));
    auto safe=safe_output(output);put(result,"output",json_object_new_string_len(safe.data(),static_cast<int>(safe.size())));return result;
}
