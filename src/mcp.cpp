#include "mcp.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <map>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t config_limit=256000;
constexpr const char *protocol="2025-06-18";
using Failure=std::runtime_error;
std::string encoded(J *value){return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);}
JsonPtr parsed(const std::string &text){
    if(text.size()>RESPONSE_LIMIT||!valid_utf8(text))throw Failure("MCP response is too large or is not UTF-8.");
    json_tokener *parser=json_tokener_new();
    json_tokener_set_flags(parser,JSON_TOKENER_STRICT);
    J *value=json_tokener_parse_ex(parser,text.c_str(),static_cast<int>(text.size()+1));
    auto parse_error=json_tokener_get_error(parser);
    bool ok=parse_error==json_tokener_success;
    size_t end=json_tokener_get_parse_end(parser);json_tokener_free(parser);
    for(size_t i=end;i<text.size();i++)if(!isspace(static_cast<unsigned char>(text[i])))ok=false;
    JsonPtr result(value);
    if(!ok){
        size_t row=1,column=1;
        for(size_t i=0;i<std::min(end,text.size());i++){
            if(text[i]=='\n'){row++;column=1;}else column++;
        }
        char message[256];
        snprintf(message,sizeof message,tr("JSON 格式错误（第 %zu 行，第 %zu 字节）：%s"),row,column,parse_error==json_tokener_success?tr("JSON 后有多余内容"):json_tokener_error_desc(parse_error));
        throw Failure(message);
    }
    if(!json_object_is_type(value,json_type_object))throw Failure(tr("MCP JSON 顶层必须是对象。"));
    return result;
}
bool string_value(J *value,bool empty=false){
    if(!json_object_is_type(value,json_type_string))return false;
    const char *text=json_object_get_string(value);
    return (empty||*text)&&strlen(text)==static_cast<size_t>(json_object_get_string_len(value))&&valid_utf8(text);
}
bool visible(std::string_view value){
    return std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32||c==127;});
}
void validate(J *servers){
    if(!json_object_is_type(servers,json_type_object)||json_object_object_length(servers)>16)throw Failure("MCP configuration must contain at most 16 servers.");
    json_object_object_foreach(servers,name,entry){
        if(!*name||strlen(name)>100||!valid_utf8(name)||!visible(name)||!json_object_is_type(entry,json_type_object))throw Failure("Invalid MCP server name or configuration.");
        const bool remote=get(entry,"url")!=nullptr;
        if(get(entry,"disabled")&&!json_object_is_type(get(entry,"disabled"),json_type_boolean))throw Failure("MCP disabled must be true or false.");
        const char *type=sget(entry,"type");
        if(*type&&strcmp(type,remote?"http":"stdio")&&!(remote&&!strcmp(type,"streamable-http")))throw Failure("Supported MCP transports are stdio and Streamable HTTP.");
        if(get(entry,"timeoutMs")&&(!json_object_is_type(get(entry,"timeoutMs"),json_type_int)||iget(entry,"timeoutMs",0)<1000||iget(entry,"timeoutMs",0)>300000))throw Failure("MCP timeoutMs must be between 1000 and 300000.");
        if(remote){
            if(!string_value(get(entry,"url"))||get(entry,"command"))throw Failure("Specify either an MCP URL or a command.");
            CURLU *url=curl_url();char *scheme=nullptr,*host=nullptr,*part=nullptr;
            bool ok=curl_url_set(url,CURLUPART_URL,sget(entry,"url"),0)==CURLUE_OK;
            curl_url_get(url,CURLUPART_SCHEME,&scheme,0);curl_url_get(url,CURLUPART_HOST,&host,0);
            ok=ok&&scheme&&host&&(!strcmp(scheme,"https")||(!strcmp(scheme,"http")&&(!strcmp(host,"localhost")||!strcmp(host,"127.0.0.1")||!strcmp(host,"[::1]"))));
            for(auto key:{CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_FRAGMENT}){if(curl_url_get(url,key,&part,0)==CURLUE_OK)ok=false;curl_free(part);part=nullptr;}
            curl_free(scheme);curl_free(host);curl_url_cleanup(url);
            if(!ok)throw Failure("MCP URL must use HTTPS, or HTTP on localhost, without embedded credentials or a fragment.");
        }else{
            if(!string_value(get(entry,"command")))throw Failure("MCP stdio requires a command.");
#ifdef _WIN32
            throw Failure("MCP stdio is currently supported on Linux. Use a remote MCP URL on Windows.");
#endif
            J *args=get(entry,"args");
            if(args&&!json_object_is_type(args,json_type_array))throw Failure("MCP args must be an array of strings.");
            for(size_t i=0;i<alen(args);i++)if(!string_value(at(args,i),true))throw Failure("Invalid MCP command argument.");
            if(get(entry,"cwd")&&!string_value(get(entry,"cwd")))throw Failure("Invalid MCP working directory.");
        }
        for(const char *key:{"env","headers"}){
            J *map=get(entry,key);if(!map)continue;
            if(!json_object_is_type(map,json_type_object))throw Failure("MCP env and headers must be string maps.");
            json_object_object_foreach(map,field,value){
                if(!*field||!visible(field)||!string_value(value,true))throw Failure("Invalid MCP environment or header value.");
                if(!strcmp(key,"env")&&strchr(field,'='))throw Failure("Invalid MCP environment variable name.");
                if(!strcmp(key,"headers")){
                    if(!visible(json_object_get_string(value))||std::any_of(field,field+strlen(field),[](unsigned char c){return !isalnum(c)&&c!='-'&&c!='_';}))throw Failure("Invalid MCP HTTP header.");
                    std::string lower=field;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
                    for(const char *reserved:{"host","content-length","transfer-encoding","connection","accept","content-type","mcp-session-id","mcp-protocol-version"})if(lower==reserved)throw Failure("MCP configuration contains a reserved HTTP header.");
                }
            }
        }
        if(encoded(entry).size()>64000)throw Failure("MCP server configuration is too large.");
    }
}
std::string config_path(App &app){return std::string(app.config_dir)+"/mcp.json";}
struct Client {
    App &app;
    std::string name,session,version;
    JsonPtr config;
    long next_id=0;
#ifndef _WIN32
    pid_t child=-1;
    int input=-1,output=-1;
    std::string pending;
#endif
    Client(App &a,const char *n,J *c):app(a),name(n),config(copy(c)){}
    ~Client(){
        close_remote();
#ifndef _WIN32
        if(input>=0)close(input);
        if(output>=0)close(output);
        if(child>0){
            kill(-child,SIGTERM);
            int status=0;double until=monotime()+0.3;
            pid_t result=0;
            while((result=waitpid(child,&status,WNOHANG))==0&&monotime()<until)usleep(10000);
            kill(-child,SIGKILL);
            if(result==0)while(waitpid(child,&status,0)<0&&errno==EINTR){}
        }
#endif
    }
    void close_remote() noexcept {
        if(session.empty()||!*sget(config.get(),"url"))return;
        try{
            CurlHandle handle(curl_easy_init());
            if(!handle)return;
            HttpHeaders headers;
            headers.append("Mcp-Session-Id: "+session);
            if(!version.empty())headers.append("MCP-Protocol-Version: "+version);
            J *configured=get(config.get(),"headers");
            if(configured){json_object_object_foreach(configured,key,value){headers.append(std::string(key)+": "+json_object_get_string(value));}}
            CurlRequest request(handle.get());
            auto discard=+[](char *,size_t size,size_t count,void *)->size_t{return size*count;};
            curl_easy_setopt(handle.get(),CURLOPT_URL,sget(config.get(),"url"));
            curl_easy_setopt(handle.get(),CURLOPT_CUSTOMREQUEST,"DELETE");
            curl_easy_setopt(handle.get(),CURLOPT_HTTPHEADER,headers.get());
            curl_easy_setopt(handle.get(),CURLOPT_CONNECTTIMEOUT_MS,500L);
            curl_easy_setopt(handle.get(),CURLOPT_TIMEOUT_MS,1000L);
            curl_easy_setopt(handle.get(),CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(handle.get(),CURLOPT_FOLLOWLOCATION,0L);
            curl_easy_setopt(handle.get(),CURLOPT_WRITEFUNCTION,discard);
            curl_easy_perform(handle.get());
        }catch(...){} // Best-effort shutdown must not interrupt local cleanup.
    }
    void check(double until){
        check_exit(app);
        progress_tick(&app);
        if(interrupted)throw Failure("MCP operation cancelled.");
        if(monotime()>until)throw Failure("MCP operation timed out. The operation was not automatically repeated.");
    }
#ifndef _WIN32
    void start(){
        int to_child[2],from_child[2];
        if(pipe2(to_child,O_CLOEXEC))throw Failure("Could not create MCP input pipe.");
        if(pipe2(from_child,O_CLOEXEC)){close(to_child[0]);close(to_child[1]);throw Failure("Could not create MCP output pipe.");}
        std::vector<std::string> arguments{sget(config.get(),"command")};
        J *args=get(config.get(),"args");for(size_t i=0;i<alen(args);i++)arguments.emplace_back(json_object_get_string(at(args,i)));
        std::vector<char *> argv;for(auto &arg:arguments)argv.push_back(arg.data());argv.push_back(nullptr);
        std::map<std::string,std::string> environment;
        // Servers receive a minimal inherited environment plus explicit env entries.
        for(const char *key:{"PATH","HOME","LANG","LC_ALL","TMPDIR","XDG_RUNTIME_DIR","DISPLAY","WAYLAND_DISPLAY"})if(const char *value=env_value(key))environment[key]=value;
        J *env=get(config.get(),"env");if(env){json_object_object_foreach(env,key,value){environment[key]=json_object_get_string(value);}}
        std::string cwd=sget(config.get(),"cwd");if(cwd.empty())cwd=app.root;
        pid_t parent=getpid();child=fork();
        if(child==0){
            signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);
            if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent||chdir(cwd.c_str()))_exit(126);
            int sink=open("/dev/null",O_WRONLY);
            if(sink<0||dup2(to_child[0],0)<0||dup2(from_child[1],1)<0||dup2(sink,2)<0)_exit(126);
            close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);if(sink>2)close(sink);
            clearenv();for(const auto &[key,value]:environment)setenv(key.c_str(),value.c_str(),1);
            execvp(argv[0],argv.data());_exit(127);
        }
        close(to_child[0]);close(from_child[1]);
        if(child<0){close(to_child[1]);close(from_child[0]);throw Failure("Could not launch MCP server.");}
        setpgid(child,child);input=to_child[1];output=from_child[0];
        fcntl(input,F_SETFL,O_NONBLOCK);fcntl(output,F_SETFL,O_NONBLOCK);
    }
    void write_message(J *message,double until){
        std::string bytes=encoded(message)+"\n";size_t used=0;
        while(used<bytes.size()){
            check(until);pollfd ready{input,POLLOUT,0};if(poll(&ready,1,50)<=0)continue;
            ssize_t count=write(input,bytes.data()+used,bytes.size()-used);
            if(count<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(count<=0)throw Failure("MCP server closed its input.");
            used+=static_cast<size_t>(count);
        }
    }
    JsonPtr read_message(double until){
        for(;;){
            check(until);auto end=pending.find('\n');
            if(end!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);return parsed(line);}
            pollfd ready{output,POLLIN,0};if(poll(&ready,1,50)<=0)continue;
            char bytes[8192];ssize_t count=read(output,bytes,sizeof bytes);
            if(count<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(count<=0)throw Failure("MCP server closed its output. Check the command and its dependencies.");
            pending.append(bytes,static_cast<size_t>(count));if(pending.size()>RESPONSE_LIMIT)throw Failure("MCP response exceeds the size limit.");
        }
    }
#endif
    JsonPtr server_reply(J *message){
        JsonPtr reply(obj());put(reply.get(),"jsonrpc",str("2.0"));put(reply.get(),"id",copy(get(message,"id")));
        if(!strcmp(sget(message,"method"),"ping"))put(reply.get(),"result",obj());
        else{J *error=obj();put(error,"code",json_object_new_int(-32601));put(error,"message",str("Client capability not supported"));put(reply.get(),"error",error);}
        return reply;
    }
    struct Transfer {
        Client &client;long id;bool notification;
        std::string bytes,event,session,content_type,error;
        JsonPtr response;
        size_t received=0;
        bool sse=false;
        void accept(const std::string &data){
            auto value=parsed(data);
            if(strcmp(sget(value.get(),"jsonrpc"),"2.0"))throw Failure("Invalid MCP JSON-RPC version.");
            if(*sget(value.get(),"method")){
                if(get(value.get(),"id")){auto reply=client.server_reply(value.get());client.http(reply.get(),0,true);}
            }else if(get(value.get(),"id")&&json_object_is_type(get(value.get(),"id"),json_type_int)&&json_object_get_int64(get(value.get(),"id"))==id)response=std::move(value);
        }
    };
    static size_t http_header(char *data,size_t size,size_t count,void *pointer){
        auto &transfer=*static_cast<Transfer *>(pointer);
        try{
            size_t length=size*count;std::string line(data,length);auto colon=line.find(':');
            if(colon!=std::string::npos){
                auto key=line.substr(0,colon);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
                auto value=line.substr(colon+1);while(!value.empty()&&isspace(static_cast<unsigned char>(value.back())))value.pop_back();size_t start=value.find_first_not_of(" \t");value=start==std::string::npos?"":value.substr(start);
                if(key=="mcp-session-id"){
                    if(value.empty()||value.size()>256||std::any_of(value.begin(),value.end(),[](unsigned char c){return c<33||c>126;}))throw Failure("Invalid MCP session ID.");
                    transfer.session=value;
                }
                if(key=="content-type"){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});transfer.content_type=value;transfer.sse=value.starts_with("text/event-stream");}
            }
            return length;
        }catch(...){transfer.error="Invalid MCP response headers.";return 0;}
    }
    static size_t http_body(char *data,size_t size,size_t count,void *pointer){
        auto &transfer=*static_cast<Transfer *>(pointer);
        try{
            if(size&&count>SIZE_MAX/size)throw Failure("MCP response exceeds the size limit.");
            size_t length=size*count;
            if(length>RESPONSE_LIMIT-transfer.received)throw Failure("MCP response exceeds the size limit.");
            transfer.received+=length;transfer.bytes.append(data,length);
            if(transfer.sse){
                for(;;){
                    auto end=transfer.bytes.find('\n');if(end==std::string::npos)break;
                    auto line=transfer.bytes.substr(0,end);transfer.bytes.erase(0,end+1);if(!line.empty()&&line.back()=='\r')line.pop_back();
                    if(line.empty()){
                        if(!transfer.event.empty()){transfer.accept(transfer.event);transfer.event.clear();}
                        if(transfer.response)return 0;
                    }else if(line.starts_with("data:")){
                        auto value=line.substr(5);if(!value.empty()&&value.front()==' ')value.erase(0,1);
                        if(!transfer.event.empty())transfer.event+='\n';
                        transfer.event+=value;
                    }
                }
            }
            return length;
        }catch(const std::exception &error){transfer.error=error.what();return 0;}catch(...){transfer.error="Could not read MCP response.";return 0;}
    }
    static int http_progress(void *pointer,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
        auto &app=*static_cast<App *>(pointer);check_exit(app);progress_tick(&app);return interrupted?1:0;
    }
    JsonPtr http(J *message,long id,bool notification){
        CurlHandle handle(curl_easy_init());
        if(!handle)throw Failure("Could not initialize MCP HTTP client.");
        std::string body=encoded(message);Transfer transfer{*this,id,notification,{},{},{},{},{},JsonPtr{},0,false};
        HttpHeaders headers;
        headers.append("Content-Type: application/json");headers.append("Accept: application/json, text/event-stream");
        if(!session.empty())headers.append("Mcp-Session-Id: "+session);
        if(!version.empty())headers.append("MCP-Protocol-Version: "+version);
        J *configured=get(config.get(),"headers");if(configured){json_object_object_foreach(configured,key,value){headers.append(std::string(key)+": "+json_object_get_string(value));}}
        CURL *curl=handle.get();CurlRequest request(curl);
        curl_easy_setopt(curl,CURLOPT_URL,sget(config.get(),"url"));curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers.get());
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body.c_str());curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,static_cast<long>(iget(config.get(),"timeoutMs",60000)));
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,http_header);curl_easy_setopt(curl,CURLOPT_HEADERDATA,&transfer);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_body);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&transfer);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,http_progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&app);curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        CURLcode code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        if(interrupted)throw Failure("MCP request cancelled. A remote operation may still be running.");
        if(!transfer.error.empty())throw Failure(transfer.error);
        if(code!=CURLE_OK&&!(code==CURLE_WRITE_ERROR&&transfer.response))throw Failure("MCP connection failed or timed out. The request was not automatically repeated.");
        if(status<200||status>=300){if(status==404)session.clear();throw Failure("MCP HTTP "+std::to_string(status)+". Check the address, authentication and server availability.");}
        if(!transfer.session.empty())session=transfer.session;
        if(notification)return JsonPtr(obj());
        if(!transfer.sse){if(!transfer.content_type.starts_with("application/json"))throw Failure("MCP server did not return JSON or an SSE stream.");transfer.accept(transfer.bytes);}
        if(!transfer.response)throw Failure("MCP response did not match the request ID.");
        return std::move(transfer.response);
    }
    JsonPtr request(const char *method,J *params=nullptr,bool notification=false){
        JsonPtr message(obj());put(message.get(),"jsonrpc",str("2.0"));put(message.get(),"method",str(method));
        long id=++next_id;if(!notification)put(message.get(),"id",json_object_new_int64(id));if(params)put(message.get(),"params",copy(params));
        JsonPtr response;
        if(*sget(config.get(),"url"))response=http(message.get(),id,notification);
#ifndef _WIN32
        else{
            double until=monotime()+iget(config.get(),"timeoutMs",60000)/1000.0;
            write_message(message.get(),until);if(notification)return JsonPtr(obj());
            for(int count=0;count<256;count++){
                response=read_message(until);
                if(strcmp(sget(response.get(),"jsonrpc"),"2.0"))throw Failure("Invalid MCP JSON-RPC version.");
                if(*sget(response.get(),"method")){
                    if(get(response.get(),"id")){auto reply=server_reply(response.get());write_message(reply.get(),until);}
                    continue;
                }
                if(json_object_is_type(get(response.get(),"id"),json_type_int)&&json_object_get_int64(get(response.get(),"id"))==id)break;
                response.reset();
            }
        }
#endif
        if(notification)return JsonPtr(obj());
        if(!response||!json_object_is_type(get(response.get(),"id"),json_type_int)||json_object_get_int64(get(response.get(),"id"))!=id)throw Failure("MCP response did not match the request ID.");
        if(get(response.get(),"error"))throw Failure("MCP server rejected "+std::string(method)+" (JSON-RPC "+std::to_string(iget(get(response.get(),"error"),"code",0))+").");
        J *result=get(response.get(),"result");if(!json_object_is_type(result,json_type_object))throw Failure("MCP result is not an object.");
        return JsonPtr(copy(result));
    }
    void initialize(bool explicit_test){
        if(!*sget(config.get(),"url")){
            if(!explicit_test){std::string preview;
                // Environment variables and HTTP headers are omitted from the preview.
                JsonPtr public_config(copy(config.get()));json_object_object_del(public_config.get(),"env");json_object_object_del(public_config.get(),"headers");preview="MCP "+name+"\n"+encoded(public_config.get());
                if(!approve(&app,preview.c_str()))throw Failure("MCP server launch was not approved.");}
#ifndef _WIN32
            start();
#endif
        }
        progress(&app,tr("正在连接 MCP…"));
        JsonPtr params(obj());put(params.get(),"protocolVersion",str(protocol));put(params.get(),"capabilities",obj());
        J *info=obj();put(info,"name",str("desk-agent"));put(info,"version",str(VERSION));put(params.get(),"clientInfo",info);
        auto result=request("initialize",params.get());version=sget(result.get(),"protocolVersion");
        if(version!="2025-06-18"&&version!="2025-03-26"&&version!="2024-11-05")throw Failure("MCP server selected an unsupported protocol version.");
        if(!get(get(result.get(),"capabilities"),"tools"))throw Failure("MCP server does not advertise tools.");
        request("notifications/initialized",nullptr,true);
    }
};
struct Tool {Client *client;std::string original;JsonPtr definition;};
struct Registry {
    std::string configuration;
    std::vector<std::unique_ptr<Client>> clients;
    std::map<std::string,Tool> tools;
};
}

J *mcp_load(App &app){
    const auto path=config_path(app);errno=0;std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),config_limit,nullptr),free);
    if(!bytes){if(errno==ENOENT)return obj();seterr(&app,"Could not read the local MCP configuration.");return nullptr;}
    try{auto document=parsed(bytes.get());J *servers=get(document.get(),"mcpServers");validate(servers);return copy(servers);}
    catch(const std::exception &error){seterr(&app,error.what());return nullptr;}
}
bool mcp_save(App &app,J *servers){
    try{
        validate(servers);JsonPtr document(obj());put(document.get(),"mcpServers",copy(servers));auto bytes=encoded(document.get());
        if(bytes.size()>config_limit)throw Failure("MCP configuration is too large.");
        if(mkdirs(app.config_dir,0700)||atomic_file(config_path(app).c_str(),bytes.data(),bytes.size(),0600))throw Failure("Could not save the local MCP configuration.");
        mcp_close(app);return true;
    }catch(const std::exception &error){seterr(&app,error.what());return false;}
}
bool mcp_import_json(App &app,const std::string &text){
    if(text.size()>config_limit){seterr(&app,tr("MCP 配置不能超过 256 KB。"));return false;}
    try{
        auto document=parsed(text);
        J *servers=get(document.get(),"mcpServers");
        if(!servers)throw Failure(tr("缺少 mcpServers 对象，请参考 MCP 配置示例。"));
        validate(servers);
        if(!json_object_object_length(servers))throw Failure(tr("mcpServers 为空，没有可导入的服务。"));
        JsonPtr existing(mcp_load(app));
        if(!existing)return false;
        json_object_object_foreach(servers,name,entry){
            if(get(existing.get(),name))throw Failure(tr("服务名称重复，请修改 JSON 中的服务名称后重试。"));
            put(existing.get(),name,copy(entry));
        }
        return mcp_save(app,existing.get());
    }catch(const std::exception &error){seterr(&app,error.what());return false;}
}
bool mcp_import(App &app,const char *path){
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path,config_limit,&length),free);
    if(!bytes){seterr(&app,tr("无法读取 MCP JSON 文件，请检查文件路径和权限（最大 256 KB）。"));return false;}
    return mcp_import_json(app,std::string(bytes.get(),length));
}
void mcp_close(App &app){delete static_cast<Registry *>(app.mcp_data);app.mcp_data=nullptr;}
bool mcp_prepare(App &app,bool explicit_test,const char *only){
    JsonPtr servers(mcp_load(app));if(!servers)return false;
    std::string configuration=encoded(servers.get());auto *previous=static_cast<Registry *>(app.mcp_data);
    if(!explicit_test&&previous&&previous->configuration==configuration)return true;
    mcp_close(app);auto registry=std::make_unique<Registry>();registry->configuration=configuration;
    try{
        json_object_object_foreach(servers.get(),name,entry){
            if(only?strcmp(only,name)!=0:bget(entry,"disabled"))continue;
            auto client=std::make_unique<Client>(app,name,entry);client->initialize(explicit_test);
            std::string cursor;std::vector<std::string> cursors;
            do{
                JsonPtr params(obj());if(!cursor.empty())put(params.get(),"cursor",str(cursor.c_str()));
                auto page=client->request("tools/list",params.get());J *list=get(page.get(),"tools");
                if(!json_object_is_type(list,json_type_array))throw Failure("MCP tools/list did not return an array.");
                for(size_t i=0;i<alen(list);i++){
                    J *tool=at(list,i),*schema=get(tool,"inputSchema");
                    if(!string_value(get(tool,"name"))||!visible(sget(tool,"name"))||!json_object_is_type(schema,json_type_object)||strcmp(sget(schema,"type"),"object"))throw Failure("Invalid MCP tool name or input schema.");
                    std::string original=sget(tool,"name"),identity=std::string(name)+"\n"+original;
                    std::unique_ptr<char,decltype(&free)> hash(sha256(identity.data(),identity.size()),free);if(!hash)throw Failure("Could not name MCP tool.");
                    std::string alias="mcp_"+std::string(hash.get(),48);
                    if(registry->tools.count(alias)||registry->tools.size()>=128)throw Failure("Duplicate MCP tool or more than 128 imported tools.");
                    JsonPtr definition(obj());put(definition.get(),"type",str("function"));J *function=obj();put(function,"name",str(alias.c_str()));
                    std::string description="MCP "+std::string(name)+" / "+original+". "+sget(tool,"description");
                    put(function,"description",str(description.c_str()));put(function,"parameters",copy(schema));put(definition.get(),"function",function);
                    if(encoded(definition.get()).size()>64000)throw Failure("MCP tool schema is too large.");
                    registry->tools.emplace(alias,Tool{client.get(),original,std::move(definition)});
                }
                cursor=sget(page.get(),"nextCursor");
                if(!cursor.empty()){if(cursors.size()>=32||std::find(cursors.begin(),cursors.end(),cursor)!=cursors.end())throw Failure("MCP tools/list pagination did not finish.");cursors.push_back(cursor);}
            }while(!cursor.empty());
            registry->clients.push_back(std::move(client));
        }
        app.mcp_data=registry.release();progress_clear(&app);return true;
    }catch(const std::exception &error){progress_clear(&app);seterr(&app,error.what());return false;}
}
void mcp_append_tools(App &app,J *tools){
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(registry)for(const auto &[name,tool]:registry->tools){(void)name;add(tools,copy(tool.definition.get()));}
}
J *mcp_tools(App &app){
    JsonPtr tools(arr());mcp_append_tools(app,tools.get());return tools.release();
}
J *mcp_call(App &app,const char *name,J *arguments){
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!registry||!json_object_is_type(arguments,json_type_object))return err("Unknown MCP tool or invalid arguments.");
    auto found=registry->tools.find(name);
    if(found==registry->tools.end())return err("Unknown MCP tool or invalid arguments.");
    auto &tool=found->second;
    std::string preview="MCP "+tool.client->name+" / "+tool.original+"\n"+encoded(arguments);
    if(!approve(&app,preview.c_str())){J *result=err("MCP operation was not approved.");put(result,"denied",json_object_new_boolean(true));return result;}
    try{
        JsonPtr params(obj());put(params.get(),"name",str(tool.original.c_str()));put(params.get(),"arguments",copy(arguments));
        auto result=tool.client->request("tools/call",params.get());
        if(bget(result.get(),"isError"))put(result.get(),"error",str("MCP tool reported a failure."));
        return result.release();
    }catch(const std::exception &error){J *result=err(error.what());mcp_close(app);return result;}
}
