#include <winsock2.h>
#include <ws2tcpip.h>
#include "windows/win.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <iostream>
#include <atomic>
#include <functional>
static int checks=0;
#define CHECK(value) do{++checks;if(!(value))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #value);}while(0)
static bool checked(J *result){if(get(result,"error")){std::cerr<<"Tool result: "<<json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN)<<" (Windows error "<<GetLastError()<<")\n";return false;}return true;}
static std::string executable;
static fs::path sandbox;
static int sequence=0;
static void environment(const wchar_t *key,const std::string &value){CHECK(SetEnvironmentVariableW(key,value.empty()?nullptr:wide(value).c_str())!=FALSE);}
struct Fixture {
    App app;
    Fixture(){
        auto root=sandbox/(L"case-"+std::to_wstring(sequence++));CHECK(!mkdirs(path_text(root).c_str(),0700));CHECK(canonical_directory(path_text(root).c_str(),app.root));
        environment(L"XDG_CONFIG_HOME",path_text(root/"config"));environment(L"XDG_STATE_HOME",path_text(root/"state"));
        for(const wchar_t *key:{L"DESK_API_KEY",L"DESK_BASE_URL",L"DESK_MODEL",L"DESK_API_TYPE",L"DESK_LANG"})CHECK(SetEnvironmentVariableW(key,nullptr)!=FALSE||GetLastError()==ERROR_ENVVAR_NOT_FOUND);
        CHECK(!store_load(&app));reset_session(&app);app.yes=true;app.max_steps=8;
        json_object_put(app.config);app.config=config_default("windows-test");put(app.config,"model",str("model-test"));put(app.config,"api_key",str("fake-windows-test-key"));
    }
    ~Fixture(){app_cleanup(&app);}
};
static J *path_args(const char *path){J *args=obj();put(args,"path",str(path));return args;}
static std::string response(const char *name=nullptr,J *arguments=nullptr,const char *text="done"){
    JsonPtr body(obj()),input(arguments);J *choices=arr(),*choice=obj(),*message=obj();put(message,"role",str("assistant"));put(message,"content",str(text));
    if(name){J *calls=arr(),*call=obj(),*function=obj();put(call,"id",str("tool-1"));put(call,"type",str("function"));put(function,"name",str(name));put(function,"arguments",str(json_object_to_json_string_ext(input.get(),JSON_C_TO_STRING_PLAIN)));put(call,"function",function);add(calls,call);put(message,"tool_calls",calls);}
    put(choice,"message",message);add(choices,choice);put(body.get(),"choices",choices);return json_object_to_json_string_ext(body.get(),JSON_C_TO_STRING_PLAIN);
}
static J *command_args(const char *command){J *args=obj();put(args,"command",str(command));return args;}
class Server {
    SOCKET listener=INVALID_SOCKET;std::jthread thread;std::mutex mutex;std::vector<std::string> replies;size_t barrier;
    std::atomic<int> finished{0};
    bool send_reply(SOCKET socket,const std::string &reply){std::string bytes="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(reply.size())+"\r\nConnection: close\r\n\r\n"+reply;while(!bytes.empty()){int n=send(socket,bytes.data(),static_cast<int>(bytes.size()),0);if(n<=0)return false;bytes.erase(0,static_cast<size_t>(n));}return true;}
    void serve(){
        std::vector<SOCKET> pending;
        for(size_t i=0;i<replies.size();i++){
            SOCKET socket=accept(listener,nullptr,nullptr);if(socket==INVALID_SOCKET){finished=-1;return;}
            DWORD timeout=7000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char *>(&timeout),sizeof timeout);
            std::string request;size_t header=std::string::npos;char bytes[4096];
            while((header=request.find("\r\n\r\n"))==std::string::npos){int n=recv(socket,bytes,sizeof bytes,0);if(n<=0){closesocket(socket);finished=-2;return;}request.append(bytes,n);if(request.size()>500000){finished=-3;closesocket(socket);return;}}
            size_t length=0;std::string headers=request.substr(0,header);std::string lower=headers;for(char &c:lower)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
            auto marker=lower.find("content-length:");if(marker!=std::string::npos)length=std::stoul(lower.substr(marker+15));
            if(length>500000){finished=-3;closesocket(socket);return;}
            while(request.size()<header+4+length){int n=recv(socket,bytes,sizeof bytes,0);if(n<=0){closesocket(socket);finished=-2;return;}request.append(bytes,n);}
            {std::lock_guard lock(mutex);bodies.push_back(request.substr(header+4,length));paths.push_back(request.substr(0,request.find("\r\n")));all_headers.push_back(headers);}
            pending.push_back(socket);
            if(pending.size()==barrier||i+1==replies.size()){
                size_t first=i+1-pending.size();for(size_t k=0;k<pending.size();k++){if(!send_reply(pending[k],replies[first+k]))finished=-4;closesocket(pending[k]);}pending.clear();
            }
        }
        finished=1;
    }
    std::vector<std::string> bodies,paths,all_headers;
public:
    std::string base;
    explicit Server(std::vector<std::string> responses,size_t group=1):replies(std::move(responses)),barrier(group){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);CHECK(listener!=INVALID_SOCKET);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(listener,reinterpret_cast<sockaddr *>(&address),sizeof address)!=SOCKET_ERROR);int size=sizeof address;CHECK(getsockname(listener,reinterpret_cast<sockaddr *>(&address),&size)!=SOCKET_ERROR);CHECK(listen(listener,8)!=SOCKET_ERROR);
        base="http://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/v1";thread=std::jthread([this]{serve();});
    }
    ~Server(){shutdown(listener,SD_BOTH);closesocket(listener);if(thread.joinable())thread.join();}
    void use(App &app){put(app.config,"base_url",str(base.c_str()));}
    void done(){double until=monotime()+8;while(!finished&&monotime()<until)Sleep(10);CHECK(finished==1);}
    J *request(size_t index){std::lock_guard lock(mutex);CHECK(index<bodies.size());return json_tokener_parse(bodies[index].c_str());}
    std::string headers(size_t index){std::lock_guard lock(mutex);return all_headers.at(index);}
};
static std::string state(Tasks &tasks,int id){JsonPtr list(tasks.snapshot());for(size_t i=0;i<alen(list.get());i++)if(iget(at(list.get(),i),"id",0)==id)return sget(at(list.get(),i),"state");return "missing";}
static bool await(Tasks &tasks,int id,const char *wanted,double seconds=10){double until=monotime()+seconds;while(monotime()<until){if(state(tasks,id)==wanted)return true;Sleep(10);}std::cerr<<"Task output: "<<tasks.output(id)<<"\n";return false;}
static bool private_acl(const fs::path &path){
    PSECURITY_DESCRIPTOR descriptor=nullptr;PACL acl=nullptr;if(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
    SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;GetSecurityDescriptorControl(descriptor,&control,&revision);
    HANDLE raw=nullptr;OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw);WinHandle token(raw);DWORD bytes=0;GetTokenInformation(token.get(),TokenUser,nullptr,0,&bytes);std::vector<unsigned char> user(bytes);GetTokenInformation(token.get(),TokenUser,user.data(),bytes,&bytes);
    unsigned char system[SECURITY_MAX_SID_SIZE];DWORD system_size=sizeof system;CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&system_size);
    bool valid=(control&SE_DACL_PROTECTED)&&acl&&acl->AceCount>=1;
    for(DWORD i=0;valid&&i<acl->AceCount;i++){void *entry=nullptr;if(!GetAce(acl,i,&entry)){valid=false;break;}auto *ace=static_cast<ACCESS_ALLOWED_ACE *>(entry);valid=ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&(EqualSid(&ace->SidStart,reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid)||EqualSid(&ace->SidStart,system));}
    LocalFree(descriptor);return valid;
}
static void test_files_config(){
    CHECK(path_inside(fs_path("C:/Work/项目"),fs::path(L"c:\\Work\\项目\\file.txt")));
    CHECK(path_inside(fs::path(L"C:\\Work\\项目"),fs_path("c:/work/项目/file.txt")));
    CHECK(!path_inside(fs_path("C:/Work"),fs::path(L"C:\\Workspace\\file.txt")));
    CHECK(!path_inside(fs_path("C:/Work"),fs::path(L"D:\\Work\\file.txt")));
    Fixture fixture;App &app=fixture.app;JsonPtr args(path_args("你好.txt"));put(args.get(),"content",str("第一版\r\nkeep\r\n"));put(args.get(),"expected_sha256",nullptr);
    JsonPtr result(tool_call(&app,"write_file",args.get()));CHECK(checked(result.get()));args.reset(path_args("你好.txt"));result.reset(tool_call(&app,"read_file",args.get()));CHECK(!strcmp(sget(result.get(),"content"),"第一版\r\nkeep\r\n"));std::string hash=sget(result.get(),"sha256");
    put(args.get(),"expected_sha256",str(hash.c_str()));put(args.get(),"old_text",str("第一版"));put(args.get(),"new_text",str("第二版 😀"));result.reset(tool_call(&app,"edit_file",args.get()));CHECK(checked(result.get()));CHECK(bget(result.get(),"verified"));
    char *backup=read_bytes(sget(result.get(),"backup"),FILE_LIMIT,nullptr);CHECK(backup&&!strcmp(backup,"第一版\r\nkeep\r\n"));free(backup);result.reset(tool_call(&app,"edit_file",args.get()));CHECK(get(result.get(),"error"));
    args.reset(path_args("."));put(args.get(),"query",str("第二版"));result.reset(tool_call(&app,"search_text",args.get()));CHECK(alen(get(result.get(),"matches"))==1);
    char out[PATH_MAX];for(const char *path:{"../outside.txt",".ENV",".git/config","file.txt:stream","CON","hello. ","C:relative.txt","\\\\server\\share\\file"})CHECK(resolve_file(&app,path,out)!=0);
    CHECK(!save_config(&app,app.config));CHECK(private_acl(fs_path(app.config_dir)/"settings.json"));
    std::string original=json_object_to_json_string_ext(app.config,JSON_C_TO_STRING_PLAIN);CHECK(save_language(app,"en"));CHECK(original==json_object_to_json_string_ext(app.config,JSON_C_TO_STRING_PLAIN));CHECK(private_acl(fs_path(app.config_dir)/"ui.json"));CHECK(set_language("zh"));
    args.reset(path_args(path_text(fs_path(app.config_dir)/"settings.json").c_str()));result.reset(tool_call(&app,"read_file",args.get()));CHECK(get(result.get(),"error"));
    CHECK(!atomic_file(path_text(fs_path(app.root)/"empty.txt").c_str(),"",0,0600));size_t size=99;char *empty=read_bytes(path_text(fs_path(app.root)/"empty.txt").c_str(),10,&size);CHECK(empty&&size==0);free(empty);
}
static void test_commands(){
    Fixture fixture;App &app=fixture.app;
    JsonPtr result(run_command(&app,"Write-Output '你好'; [Console]::Error.WriteLine('stderr'); exit 7",10000));CHECK(iget(result.get(),"exit_code",0)==7);CHECK(strstr(sget(result.get(),"output"),"你好"));CHECK(strstr(sget(result.get(),"output"),"stderr"));
    environment(L"DESK_LEAK_KEY","secret-never-in-child");result.reset(run_command(&app,"if(Test-Path Env:DESK_LEAK_KEY){exit 9}; Write-Output 'filtered'",10000));CHECK(iget(result.get(),"exit_code",-1)==0);CHECK(strstr(sget(result.get(),"output"),"filtered"));environment(L"DESK_LEAK_KEY","");
    result.reset(run_command(&app,"Write-Output ('x' * 20000)",10000));CHECK(bget(result.get(),"truncated"));CHECK(json_object_get_string_len(get(result.get(),"output"))==16000);
    result.reset(run_command(&app,"Start-Sleep -Seconds 10",200));CHECK(bget(result.get(),"timed_out"));
    std::jthread stop([]{Sleep(200);interrupted=SIGINT;});result.reset(run_command(&app,"Start-Sleep -Seconds 30",10000));stop.join();CHECK(get(result.get(),"error"));CHECK(interrupted==SIGINT);interrupted=0;
    result.reset(run_command(&app,"$p=Start-Process -FilePath ($PSHOME+'\\powershell.exe') -ArgumentList '-NoProfile','-NonInteractive','-Command','Start-Sleep -Seconds 30' -PassThru; Set-Content -Path child.pid -Value $p.Id; exit 0",10000));CHECK(iget(result.get(),"exit_code",-1)==0);
    char *pid=read_bytes(path_text(fs_path(app.root)/"child.pid").c_str(),100,nullptr);CHECK(pid);DWORD id=static_cast<DWORD>(strtoul(pid,nullptr,10));free(pid);WinHandle child(OpenProcess(SYNCHRONIZE,FALSE,id));CHECK(!child||WaitForSingleObject(child.get(),2000)==WAIT_OBJECT_0);
}
static void test_api_loops(){
    Fixture fixture;App &app=fixture.app;J *write=path_args("model.txt");put(write,"content",str("模型文件"));put(write,"expected_sha256",nullptr);
    Server server({response("write_file",write),response("read_file",path_args("model.txt")),response()});server.use(app);CHECK(!run_task(&app,"创建并读取文件"));server.done();
    JsonPtr first(server.request(0));CHECK(!strcmp(sget(first.get(),"model"),"model-test"));CHECK(!get(first.get(),"system"));J *messages=get(first.get(),"messages");CHECK(alen(messages)==1&&!strcmp(sget(at(messages,0),"role"),"user"));CHECK(!strcmp(sget(at(messages,0),"content"),"创建并读取文件"));CHECK(server.headers(0).find("Authorization: Bearer fake-windows-test-key")!=std::string::npos);
    reset_session(&app);put(app.config,"protocol",str("anthropic"));
    Server native({"{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"signed\"},{\"type\":\"tool_use\",\"id\":\"read\",\"name\":\"read_file\",\"input\":{\"path\":\"model.txt\"}}]}","{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}"});native.use(app);CHECK(!run_task(&app,"Read model.txt"));native.done();JsonPtr follow(native.request(1));CHECK(!get(follow.get(),"system"));auto serialized=std::string(json_object_to_json_string_ext(follow.get(),JSON_C_TO_STRING_PLAIN));CHECK(serialized.find("signed")!=std::string::npos);CHECK(serialized.find("tool_result")!=std::string::npos);CHECK(native.headers(0).find("x-api-key: fake-windows-test-key")!=std::string::npos);
}
static void test_parallel(){
    Fixture fixture;App &app=fixture.app;Server barrier({response(),response()},2);barrier.use(app);
    {Tasks tasks(executable);int a=tasks.start(app,"alpha"),b=tasks.start(app,"beta");CHECK(a==1&&b==2);CHECK(await(tasks,a,"已完成"));CHECK(await(tasks,b,"已完成"));JsonPtr first(barrier.request(0)),second(barrier.request(1));CHECK(alen(get(first.get(),"messages"))==1);CHECK(alen(get(second.get(),"messages"))==1);CHECK(!get(first.get(),"system"));}barrier.done();
    J *write=path_args("approved.txt");put(write,"content",str("approved"));put(write,"expected_sha256",nullptr);Server approvals({response("write_file",write),response(),response()});approvals.use(app);app.yes=false;
    {Tasks tasks(executable);int id=tasks.start(app,"create approved.txt");CHECK(id==1);CHECK(await(tasks,id,"等待确认"));CHECK(!fs::exists(fs_path(app.root)/"approved.txt"));CHECK(tasks.decide(id,true));CHECK(await(tasks,id,"已完成"));CHECK(fs::exists(fs_path(app.root)/"approved.txt"));put(app.config,"model",str("different-parent-model"));CHECK(set_language("en"));CHECK(tasks.resume(id,"followup"));CHECK(await(tasks,id,"Completed"));CHECK(set_language("zh"));JsonPtr request(approvals.request(2));CHECK(!strcmp(sget(request.get(),"model"),"model-test"));CHECK(!get(request.get(),"system"));CHECK(tasks.output(id).find("fake-windows-test-key")==std::string::npos);}approvals.done();
    put(app.config,"model",str("model-test"));app.yes=true;Server cancel({response("run_shell",command_args("Start-Sleep -Seconds 30")),response()});cancel.use(app);
    {Tasks tasks(executable);int a=tasks.start(app,"long task");CHECK(a==1);double until=monotime()+8;while(tasks.output(a).find("正在执行命令…")==std::string::npos&&monotime()<until)Sleep(10);CHECK(tasks.output(a).find("正在执行命令…")!=std::string::npos);int b=tasks.start(app,"independent task");CHECK(b==2);CHECK(tasks.cancel(a));CHECK(await(tasks,a,"已取消"));CHECK(await(tasks,b,"已完成"));}cancel.done();
}
static void test_model_switch(){
    Fixture fixture;App &app=fixture.app;
    Server original({response()});original.use(app);put(app.config,"model",str("original-model"));CHECK(!save_config(&app,app.config));
    Server selected({"{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"selected done\"}]}"});
    JsonPtr next(copy(app.config));put(next.get(),"profile",str("selected"));put(next.get(),"model",str("selected-model"));put(next.get(),"protocol",str("anthropic"));put(next.get(),"api_key",str("selected-test-key"));put(next.get(),"base_url",str(selected.base.c_str()));
    CHECK(!save_config(&app,next.get()));CHECK(!activate(&app,"windows-test"));
    {Tasks tasks(executable);int id=tasks.start(app,"original work");CHECK(id>0);CHECK(await(tasks,id,"已完成"));
        CHECK(!activate(&app,"selected"));JsonPtr settings(conversation_settings(app,false));
        CHECK(tasks.resume(id,"selected work",settings.get()));CHECK(await(tasks,id,"已完成"));
    }
    original.done();selected.done();JsonPtr request(selected.request(0));
    CHECK(!strcmp(sget(request.get(),"model"),"selected-model"));CHECK(alen(get(request.get(),"messages"))==1);CHECK(!get(request.get(),"system"));
    CHECK(selected.headers(0).find("x-api-key: selected-test-key")!=std::string::npos);CHECK(selected.headers(0).find("Authorization:")==std::string::npos);
    J *message=obj();put(message,"role",str("user"));put(message,"content",str("keep this context"));add(app.messages,message);
    auto revision=app.session_revision;CHECK(!activate(&app,"selected"));CHECK(app.session_revision==revision&&alen(app.messages)==1);
    App loaded;CHECK(!store_load(&loaded));CHECK(!strcmp(sget(loaded.config,"profile"),"selected"));CHECK(!strcmp(sget(loaded.config,"model"),"selected-model"));app_cleanup(&loaded);
}
static void test_removed_tasks(){
    Fixture fixture;App &app=fixture.app;
    Server server(std::vector<std::string>(34,response()));server.use(app);
    {Tasks tasks(executable);
        for(int i=0;i<34;i++){
            int id=tasks.start(app,"temporary conversation");CHECK(id>0);CHECK(await(tasks,id,"已完成"));
            CHECK(tasks.remove(id));CHECK(!tasks.remove(id));CHECK(!tasks.resume(id,"deleted context"));
            JsonPtr list(tasks.snapshot());CHECK(alen(list.get())==0);
        }
    }server.done();
    Server active({response("run_shell",command_args("Set-Content -Path removed.pid -Value $PID; Start-Sleep -Seconds 30"))});active.use(app);
    {Tasks tasks(executable);int id=tasks.start(app,"remove active conversation");CHECK(id>0);
        auto path=fs_path(app.root)/"removed.pid";double until=monotime()+8;DWORD number=0;
        while(!number&&monotime()<until){
            tasks.refresh();char *pid=read_bytes(path_text(path).c_str(),100,nullptr);
            if(pid){number=static_cast<DWORD>(strtoul(pid,nullptr,10));free(pid);}if(!number)Sleep(10);
        }
        CHECK(number>0);
        WinHandle child(OpenProcess(SYNCHRONIZE,FALSE,number));CHECK(child);
        CHECK(tasks.remove(id));JsonPtr list(tasks.snapshot());CHECK(alen(list.get())==0);
        until=monotime()+5;while(WaitForSingleObject(child.get(),0)!=WAIT_OBJECT_0&&monotime()<until){tasks.refresh();Sleep(10);}
        CHECK(WaitForSingleObject(child.get(),0)==WAIT_OBJECT_0);
    }active.done();
}
int main(int argc,char **argv){
    platform_initialize();WSADATA sockets{};if(WSAStartup(MAKEWORD(2,2),&sockets))return 1;curl_global_init(CURL_GLOBAL_DEFAULT);
    try{
        CHECK(argc==2);executable=argv[1];wchar_t temporary[32768];CHECK(GetTempPathW(32768,temporary)>0);char id[33];id_string(id);sandbox=fs::path(temporary)/(L"desk-test-你好-"+wide(id));CHECK(!mkdirs(path_text(sandbox).c_str(),0700));
        test_files_config();test_commands();test_api_loops();test_parallel();test_model_switch();test_removed_tasks();
        std::cout<<"PASS: "<<checks<<" Windows checks (UTF-8 files, ACL, PowerShell, cancellation, APIs, parallel tasks, approval, resume).\n";
        fs::remove_all(sandbox);curl_global_cleanup();WSACleanup();return 0;
    }catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<"\n";curl_global_cleanup();WSACleanup();return 1;}
}
