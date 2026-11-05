#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <pty.h>
#include <termios.h>
#include <algorithm>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int checks=0;
static void cancel_command(int number){(void)number;interrupted=SIGINT;}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static char sandbox[PATH_MAX];
static int sequence=0;
static App fresh(void){
    App a={
        .yes=true,.shell=true,.max_steps=8,.state="等待任务"
    };
    char suffix[64];
    snprintf(suffix,sizeof suffix,"/case-%d",sequence++);
    CHECK(path_suffix(a.root,sandbox,suffix)==0);
    CHECK(mkdirs(a.root,0700)==0);
    char cfg[PATH_MAX];
    CHECK(path_suffix(cfg,a.root,"/config")==0);
    CHECK(setenv("XDG_CONFIG_HOME",cfg,1)==0);
    unsetenv("DESK_API_KEY");
    unsetenv("DESK_BASE_URL");
    unsetenv("DESK_MODEL");
    unsetenv("DESK_API_TYPE");
    CHECK(store_load(&a)==0);
    reset_session(&a);
    return a;
}
static J *config(const char *name,const char *protocol){
    J *c=config_default(name);
    put(c,"model",str("model-test"));
    put(c,"api_key",str("fake-test-key"));
    put(c,"protocol",str(protocol));
    put(c,"base_url",str(!strcmp(protocol,"anthropic")?"https://api.anthropic.com/v1":"https://example.com/v1"));
    return c;
}
static J *args(const char *path){
    J *v=obj();
    put(v,"path",str(path));
    return v;
}
static void test_utf8(void){
    CHECK(valid_utf8("中文 English 😀"));
    CHECK(valid_utf8(""));
    CHECK(!valid_utf8("\xc0\xaf"));
    CHECK(!valid_utf8("\xed\xa0\x80"));
    CHECK(!valid_utf8("\xf4\x90\x80\x80"));
    CHECK(!valid_utf8("\xe4\xb8"));
    CHECK(!valid_utf8("\x80"));
}
static void test_files(void){
    App a=fresh();
    J *v=args("你好.txt");
    put(v,"content",str("第一版"));
    put(v,"expected_sha256",NULL);
    J *r=tool_call(&a,"write_file",v);
    CHECK(!get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args("你好.txt");
    r=tool_call(&a,"read_file",v);
    CHECK(!strcmp(sget(r,"content"),"第一版"));
    char *digest=strdup(sget(r,"sha256"));
    json_object_put(r);
    json_object_put(v);
    v=args("你好.txt");
    put(v,"content",str("第二版"));
    put(v,"expected_sha256",str(digest));
    r=tool_call(&a,"write_file",v);
    CHECK(!get(r,"error"));
    char *backup=read_bytes(sget(r,"backup"),FILE_LIMIT,NULL);
    CHECK(backup&&!strcmp(backup,"第一版"));
    free(backup);
    json_object_put(r);
    r=tool_call(&a,"write_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(digest);
    v=args("../outside");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args(".env");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    char link[PATH_MAX];
    CHECK(!path_suffix(link,a.root,"/escape"));
    CHECK(!symlink("/etc",link));
    v=args("escape/passwd");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    char *hash=sha256("abc",3);
    CHECK(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    free(hash);
    app_cleanup(&a);
}
static void fixture(App *a,const char *name,const void *data,size_t length){
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a->root,name));
    CHECK(!atomic_file(path,data,length,0600));
}
static void test_search_and_edit(void){
    App a=fresh();
    const char *original="标题=旧值\r\n保留这一行\r\n";
    fixture(&a,"/文档.txt",original,strlen(original));
    fixture(&a,"/.env","旧值",strlen("旧值"));
    const char binary[]={ 'x', (char)0xff, '\n' };
    fixture(&a,"/binary",binary,sizeof binary);
    char ignored[PATH_MAX],link[PATH_MAX];
    CHECK(!path_suffix(ignored,a.root,"/node_modules"));
    CHECK(!mkdirs(ignored,0700));
    fixture(&a,"/node_modules/dependency","旧值",strlen("旧值"));
    CHECK(!path_suffix(link,a.root,"/alias.txt"));
    CHECK(!symlink("文档.txt",link));
    J *v=args(".");
    put(v,"query",str("旧值"));
    J *r=tool_call(&a,"search_text",v);
    CHECK(!get(r,"error"));
    CHECK(alen(get(r,"matches"))==1);
    CHECK(!strcmp(sget(at(get(r,"matches"),0),"path"),"文档.txt"));
    CHECK(iget(at(get(r,"matches"),0),"line",0)==1);
    CHECK(iget(r,"skipped",0)>=4);
    CHECK(!bget(r,"truncated"));
    json_object_put(r);
    put(v,"query",str(""));
    r=tool_call(&a,"search_text",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args("文档.txt");
    r=tool_call(&a,"read_file",v);
    char *hash=strdup(sget(r,"sha256"));
    json_object_put(r);
    put(v,"old_text",str("旧值"));
    put(v,"new_text",str("新值"));
    put(v,"expected_sha256",str(hash));
    r=tool_call(&a,"edit_file",v);
    CHECK(!get(r,"error")&&bget(r,"verified"));
    char *backup=read_bytes(sget(r,"backup"),FILE_LIMIT,NULL);
    CHECK(backup&&!strcmp(backup,original));
    free(backup);
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/文档.txt"));
    char *updated=read_bytes(path,FILE_LIMIT,NULL);
    CHECK(updated&&!strcmp(updated,"标题=新值\r\n保留这一行\r\n"));
    char *actual_hash=sha256(updated,strlen(updated));
    CHECK(!strcmp(sget(r,"sha256"),actual_hash));
    free(actual_hash);
    free(updated);
    json_object_put(r);
    struct stat st;
    CHECK(!stat(path,&st)&&(st.st_mode&0777)==0600);
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    put(v,"expected_sha256",NULL);
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(hash);
    fixture(&a,"/overlap.txt","aaa",3);
    v=args("overlap.txt");
    put(v,"old_text",str("aa"));
    put(v,"new_text",str("z"));
    hash=sha256("aaa",3);
    put(v,"expected_sha256",str(hash));
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    put(v,"old_text",str("aaa"));
    put(v,"new_text",str(""));
    r=tool_call(&a,"edit_file",v);
    CHECK(!get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(hash);
    CHECK(!path_suffix(path,a.root,"/overlap.txt"));
    size_t n=99;
    updated=read_bytes(path,FILE_LIMIT,&n);
    CHECK(updated&&n==0);
    free(updated);
    char many[2048]="";
    for(int i=0;i<60;i++)strcat(many,"匹配\n");
    fixture(&a,"/many.txt",many,strlen(many));
    v=args("many.txt");
    put(v,"query",str("匹配"));
    r=tool_call(&a,"search_text",v);
    CHECK(alen(get(r,"matches"))==50&&bget(r,"truncated"));
    CHECK(iget(at(get(r,"matches"),49),"line",0)==50);
    json_object_put(r);
    json_object_put(v);
    app_cleanup(&a);
}
static void test_config(void){
    App a=fresh();
    J *one=config("日常助手","openai"),*two=config("备用","anthropic");
    CHECK(!save_config(&a,one));
    put(two,"api_key",str("different-native-key"));
    put(two,"max_tokens",json_object_new_int(8192));
    CHECK(!save_config(&a,two));
    CHECK(!strcmp(sget(a.config,"profile"),"备用"));
    CHECK(!activate(&a,"日常助手"));
    CHECK(!strcmp(sget(a.config,"protocol"),"openai"));
    CHECK(!strcmp(sget(a.config,"api_key"),"fake-test-key"));
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.config_dir,"/settings.json"));
    struct stat st;
    CHECK(!stat(path,&st));
    CHECK((st.st_mode&0777)==0600);
    J *arguments=args(path),*result=tool_call(&a,"read_file",arguments);
    CHECK(get(result,"error"));
    json_object_put(result);
    json_object_put(arguments);
    J *bad=copy(two);
    put(bad,"base_url",str("http://remote.example/v1"));
    CHECK(config_valid(&a,bad)!=0);
    put(bad,"base_url",str("https://user:password@example.com/v1"));
    CHECK(config_valid(&a,bad)!=0);
    json_object_put(bad);
    App b={
        0
    };
    CHECK(!store_load(&b));
    CHECK(!strcmp(sget(b.config,"profile"),"日常助手"));
    CHECK(iget(get(get(b.store,"profiles"),"备用"),"max_tokens",0)==8192);
    app_cleanup(&b);
    json_object_put(one);
    json_object_put(two);
    app_cleanup(&a);
}
static void test_migration(void){
    App a=fresh();
    char toml[PATH_MAX],auth[PATH_MAX];
    CHECK(!mkdirs(a.config_dir,0700));
    CHECK(!path_suffix(toml,a.config_dir,"/config.toml"));
    CHECK(!path_suffix(auth,a.config_dir,"/auth.json"));
    const char *text="active_profile = \"claude\"\n\n[profiles.\"claude\"]\nbase_url = \"https://api.anthropic.com/v1\"\nmodel = \"claude-test\"\nprotocol = \"anthropic\"\nmax_tokens = 8192\nanthropic_version = \"2023-06-01\"\n";
    CHECK(!atomic_file(toml,text,strlen(text),0600));
    const char *keys="{\"profiles\":{\"claude\":\"legacy-secret\"}}";
    CHECK(!atomic_file(auth,keys,strlen(keys),0600));
    App b={
        0
    };
    CHECK(!store_load(&b));
    CHECK(!strcmp(sget(b.config,"api_key"),"legacy-secret"));
    CHECK(iget(b.config,"max_tokens",0)==8192);
    CHECK(!strcmp(sget(b.config,"protocol"),"anthropic"));
    CHECK(!save_config(&b,b.config));
    char *unchanged=read_bytes(toml,262144,NULL);
    CHECK(unchanged&&!strcmp(unchanged,text));
    free(unchanged);
    app_cleanup(&a);
    app_cleanup(&b);
}
static bool alive(pid_t pid){
    char path[64];
    snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);
    FILE *f=fopen(path,"r");
    if(!f)return false;
    char line[2048];
    char *result=fgets(line,sizeof line,f);
    fclose(f);
    if(!result)return false;
    char *p=strrchr(line,')');
    return p&&p[2]!='Z'&&p[2]!='X';
}
static void test_process(void){
    App a=fresh();
    J *r=run_command(&a,"printf '你好'; printf ' error' >&2; exit 7",2000);
    CHECK(iget(r,"exit_code",0)==7);
    CHECK(!strcmp(sget(r,"output"),"你好 error"));
    json_object_put(r);
    setenv("DESK_API_KEY","dont-echo-this",1);
    r=run_command(&a,"printf '%s' \"${DESK_API_KEY-unset}\"",2000);
    CHECK(!strcmp(sget(r,"output"),"unset"));
    json_object_put(r);
    unsetenv("DESK_API_KEY");
    r=run_command(&a,"head -c 17000 /dev/zero",2000);
    CHECK(bget(r,"truncated"));
    CHECK(json_object_get_string_len(get(r,"output"))==16000);
    json_object_put(r);
    r=run_command(&a,"sleep 10 & echo $! > child.pid; wait",50);
    CHECK(bget(r,"timed_out"));
    CHECK(iget(r,"exit_code",0)<0);
    json_object_put(r);
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/child.pid"));
    char *pid_text=read_bytes(path,100,NULL);
    CHECK(pid_text);
    pid_t pid=(pid_t)strtol(pid_text,NULL,10);
    free(pid_text);
    double until=monotime()+2;
    while(alive(pid)&&monotime()<until)usleep(1000);
    CHECK(!alive(pid));
    struct sigaction action{},previous;
    action.sa_handler=cancel_command;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGALRM,&action,&previous));
    alarm(1);
    r=run_command(&a,"sleep 30",30000);
    alarm(0);
    CHECK(interrupted==SIGINT);
    CHECK(get(r,"error"));
    json_object_put(r);
    interrupted=0;
    CHECK(!sigaction(SIGALRM,&previous,NULL));
    app_cleanup(&a);
}
static void test_context_and_validation(void){
    App a=fresh();
    for(int i=0;i<10;i++){
        J *m=obj();
        put(m,"role",str("user"));
        put(m,"content",str("old"));
        add(a.messages,m);
        m=obj();
        put(m,"role",str("assistant"));
        put(m,"content",str("done"));
        add(a.messages,m);
    }
    J *m=obj();
    put(m,"role",str("user"));
    put(m,"content",str("current"));
    add(a.messages,m);
    J *defs=schemas(&a),*ctx=context(&a,defs);
    CHECK(ctx);
    CHECK(alen(ctx)==13);
    CHECK(alen(a.messages)==21);
    CHECK(!strcmp(sget(at(ctx,alen(ctx)-1),"content"),"current"));
    json_object_put(ctx);
    json_object_put(defs);
    J *data=json_tokener_parse("{\"stop_reason\":\"max_tokens\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\",\"name\":\"write_file\",\"input\":{}}]}");
    CHECK(!native_response(&a,data));
    json_object_put(data);
    data=json_tokener_parse("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\",\"name\":\"read_file\",\"input\":\"bad\"}]}");
    CHECK(!native_response(&a,data));
    json_object_put(data);
    app_cleanup(&a);
}
static void test_tool_configuration(){
    App a=fresh();a.shell=true;a.desktop=true;
    JsonPtr enabled(schemas(&a));CHECK(alen(enabled.get())==9);
    // Mutating one caller's schema must not change validation or other callers.
    json_object_object_del(get(get(get(at(enabled.get(),0),"function"),"parameters"),"properties"),"path");
    JsonPtr untouched(schemas(&a));
    CHECK(get(get(get(get(at(untouched.get(),0),"function"),"parameters"),"properties"),"path"));
    JsonPtr empty(obj()),result(tool_call(&a,"list_files",empty.get()));CHECK(tool_failed(result.get()));
    a.shell=false;a.desktop=false;
    JsonPtr disabled(schemas(&a));CHECK(alen(disabled.get())==5);
    JsonPtr shell_args(obj());put(shell_args.get(),"command",str("exit 0"));
    result.reset(tool_call(&a,"run_shell",shell_args.get()));CHECK(tool_failed(result.get()));
    JsonPtr desktop_args(obj());put(desktop_args.get(),"app",str(""));
    result.reset(tool_call(&a,"desktop_snapshot",desktop_args.get()));CHECK(tool_failed(result.get()));
    a.shell=true;enabled.reset(schemas(&a));CHECK(alen(enabled.get())==6);
    a.shell=false;enabled.reset(schemas(&a));CHECK(alen(enabled.get())==5);
    app_cleanup(&a);
}
/* A local HTTP server; no external model or real credential. */ static pid_t fake_server(App *a,const char **responses,int count,int http_status=200,int delay_ms=0,bool keep_alive=false){
    int fd=socket(AF_INET,SOCK_STREAM,0);
    CHECK(fd>=0);
    struct sockaddr_in address{};
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(!bind(fd,(struct sockaddr *)&address,sizeof address));
    socklen_t length=sizeof address;
    CHECK(!getsockname(fd,(struct sockaddr *)&address,&length));
    CHECK(!listen(fd,4));
    char base[128];
    snprintf(base,sizeof base,"http://127.0.0.1:%d/v1",ntohs(address.sin_port));
    put(a->config,"base_url",str(base));
    pid_t child=fork();
    CHECK(child>=0);
    if(!child){
        alarm(12);
        FILE *stream=nullptr;
        for(int i=0;i<count;i++){
            if(!stream){
                int client=accept(fd,NULL,NULL);
                if(client<0)_exit(2);
                stream=fdopen(client,"r+");
                if(!stream)_exit(2);
            }
            char *line=NULL;
            size_t cap=0;
            long content_length=0;
            char request_path[1024]="",method[16]="";
            J *capture=obj();
            if(getline(&line,&cap,stream)<0)_exit(2);
            sscanf(line,"%15s %1023s",method,request_path);
            put(capture,"path",str(request_path));put(capture,"method",str(method));
            while(getline(&line,&cap,stream)>0&&strcmp(line,"\r\n")){
                if(!strncasecmp(line,"Content-Length:",15))content_length=strtol(line+15,NULL,10);
                if(!strncasecmp(line,"Authorization:",14))put(capture,"authorization",str(line));
                if(!strncasecmp(line,"x-api-key:",10))put(capture,"key",str(line));
                if(!strncasecmp(line,"anthropic-version:",18))put(capture,"version",str(line));
            }
            free(line);
            if(content_length<0||content_length>500000)_exit(2);
            std::string body(static_cast<size_t>(content_length),'\0');
            if(content_length&&fread(body.data(),1,body.size(),stream)!=body.size())_exit(2);
            J *parsed=json_tokener_parse(body.c_str());
            put(capture,"body",parsed);
            char path[PATH_MAX],suffix[64];
            snprintf(suffix,sizeof suffix,"/request-%d.json",i);
            if(path_suffix(path,a->root,suffix))_exit(2);
            const char *data=json_object_to_json_string(capture);
            if(atomic_file(path,data,strlen(data),0600))_exit(2);
            json_object_put(capture);
            if(delay_ms)usleep(static_cast<useconds_t>(delay_ms)*1000);
            bool reuse=keep_alive&&i+1<count;
            fprintf(stream,"HTTP/1.1 %d Test\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s",http_status,strlen(responses[i]),reuse?"keep-alive":"close",responses[i]);
            fflush(stream);
            if(!reuse){fclose(stream);stream=nullptr;}
        }
        close(fd);
        _exit(0);
    }
    close(fd);
    return child;
}
static J *capture(App *a,int i){
    char path[PATH_MAX],suffix[64];
    snprintf(suffix,sizeof suffix,"/request-%d.json",i);
    CHECK(!path_suffix(path,a->root,suffix));
    char *text=read_bytes(path,500000,NULL);
    CHECK(text);
    J *j=json_tokener_parse(text);
    free(text);
    CHECK(j);
    return j;
}
static void server_done(pid_t child){
    int status;
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void test_http_reuse(){
    App a=fresh();json_object_put(a.config);a.config=config("reuse","openai");
    const char *responses[]={"{\"ok\":true}","{\"ok\":true}","{\"ok\":true}","{\"ok\":true}"};
    // The fixture accepts one TCP connection for all four requests.
    pid_t server=fake_server(&a,responses,4,200,0,true);
    JsonPtr payload(obj());put(payload.get(),"request",str("first"));
    JsonPtr result(api_request(&a,a.config,"/chat/completions",payload.get()));CHECK(bget(result.get(),"ok"));
    put(a.config,"protocol",str("anthropic"));put(a.config,"api_key",str("native-test-key"));
    put(a.config,"anthropic_version",str("2023-06-01"));put(a.config,"max_tokens",json_object_new_int(4096));
    put(payload.get(),"request",str("second"));
    result.reset(api_request(&a,a.config,"/messages",payload.get()));CHECK(bget(result.get(),"ok"));
    result.reset(api_request(&a,a.config,"/models",nullptr));CHECK(bget(result.get(),"ok"));
    put(a.config,"protocol",str("openai"));put(a.config,"api_key",str("changed-test-key"));
    result.reset(api_request(&a,a.config,"/models",nullptr));CHECK(bget(result.get(),"ok"));
    server_done(server);
    for(int i=0;i<4;i++){
        JsonPtr request(capture(&a,i));bool native=i==1||i==2;
        CHECK(!strcmp(sget(request.get(),"method"),i<2?"POST":"GET"));
        CHECK((get(request.get(),"body")!=nullptr)==(i<2));
        CHECK((get(request.get(),"authorization")!=nullptr)==!native);
        CHECK((get(request.get(),"key")!=nullptr)==native);
        CHECK((get(request.get(),"version")!=nullptr)==native);
        if(i==0)CHECK(strstr(sget(request.get(),"authorization"),"fake-test-key"));
        if(native)CHECK(strstr(sget(request.get(),"key"),"native-test-key"));
        if(i==3)CHECK(strstr(sget(request.get(),"authorization"),"changed-test-key"));
        if(i<2)CHECK(!strcmp(sget(get(request.get(),"body"),"request"),i?"second":"first"));
    }
    app_cleanup(&a);
}
static J *model_call(const char *id,const char *name,J *arguments){
    JsonPtr input(arguments);
    J *call=obj(),*fn=obj();
    put(call,"id",str(id));
    put(call,"type",str("function"));
    put(fn,"name",str(name));
    put(fn,"arguments",str(json_object_to_json_string_ext(input.get(),JSON_C_TO_STRING_PLAIN)));
    put(call,"function",fn);
    return call;
}
static std::string model_reply(J *calls,const char *text=""){
    JsonPtr response(obj());
    J *choices=arr(),*choice=obj(),*message=obj();
    put(message,"role",str("assistant"));
    put(message,"content",str(text));
    if(calls)put(message,"tool_calls",calls);
    put(choice,"message",message);
    add(choices,choice);
    put(response.get(),"choices",choices);
    return json_object_to_json_string_ext(response.get(),JSON_C_TO_STRING_PLAIN);
}
static std::string model_tool(const char *id,const char *name,J *arguments){
    J *calls=arr();
    add(calls,model_call(id,name,arguments));
    return model_reply(calls);
}
static J *command(const char *text){
    J *v=obj();
    put(v,"command",str(text));
    return v;
}
static int workflow(App *a,const std::vector<std::string> &replies){
    std::vector<const char *> responses;
    for(const auto &reply:replies)responses.push_back(reply.c_str());
    json_object_put(a->config);
    a->config=config("test","openai");
    pid_t child=fake_server(a,responses.data(),static_cast<int>(responses.size()));
    int result=run_task(a,"完成任务并检查实际结果");
    if(result)fprintf(stderr,"Workflow stopped: %s\n",a->error);
    server_done(child);
    return result;
}
static void test_edit_workflow(void){
    App a=fresh();
    const char *original="欢迎：旧内容\n保留这行\n";
    fixture(&a,"/welcome.txt",original,strlen(original));
    char *hash=sha256(original,strlen(original));
    J *search=args(".");
    put(search,"query",str("旧内容"));
    J *edit=args("welcome.txt");
    put(edit,"old_text",str("旧内容"));
    put(edit,"new_text",str("新的欢迎语 😀"));
    put(edit,"expected_sha256",str(hash));
    free(hash);
    CHECK(!workflow(&a,{
        model_tool("find","search_text",search),
        model_tool("read","read_file",args("welcome.txt")),
        model_tool("edit","edit_file",edit),
        model_tool("check","read_file",args("welcome.txt")),
        model_reply(nullptr,"已修改并读取核对。")
    }));
    JsonPtr request(capture(&a,4));
    J *messages=get(get(request.get(),"body"),"messages");
    const char *result=sget(at(messages,alen(messages)-1),"content");
    CHECK(strstr(result,"新的欢迎语 😀")&&strstr(result,"保留这行"));
    request.reset(capture(&a,3));
    messages=get(get(request.get(),"body"),"messages");
    JsonPtr written(json_tokener_parse(sget(at(messages,alen(messages)-1),"content")));
    CHECK(bget(written.get(),"verified"));
    CHECK(*sget(written.get(),"backup"));
    app_cleanup(&a);
}
static void test_recovery_and_loops(void){
    App a=fresh();
    CHECK(workflow(&a,{
        model_tool("bad","run_shell",command("exit 7")),
        model_tool("unrelated","list_files",args(".")),
        model_reply(nullptr,"False success")
    })!=0);
    CHECK(!strcmp(a.state,"失败"));
    CHECK(!strstr(json_object_to_json_string(a.messages),"False success"));
    app_cleanup(&a);
    a=fresh();
    J *write=args("ready.txt");
    put(write,"content",str("ready"));
    put(write,"expected_sha256",nullptr);
    CHECK(!workflow(&a,{
        model_tool("missing","run_shell",command("test -f ready.txt")),
        model_tool("create","write_file",write),
        model_tool("retry","run_shell",command("test -f ready.txt")),
        model_reply(nullptr,"已创建并核对。")
    }));
    CHECK(!strcmp(a.state,"本轮结束"));
    app_cleanup(&a);
    a=fresh();
    fixture(&a,"/stable.txt","stable",6);
    write=args("must-not-exist.txt");
    put(write,"content",str("not executed"));
    put(write,"expected_sha256",nullptr);
    J *last=arr();
    add(last,model_call("read3","read_file",args("stable.txt")));
    add(last,model_call("pending","write_file",write));
    CHECK(workflow(&a,{
        model_tool("read1","read_file",args("stable.txt")),
        model_tool("read2","read_file",args("stable.txt")),
        model_reply(last)
    })!=0);
    CHECK(strstr(a.error,"相同工具"));
    CHECK(strstr(sget(at(a.messages,alen(a.messages)-1),"content"),"未确认执行"));
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/must-not-exist.txt"));
    CHECK(access(path,F_OK)!=0&&errno==ENOENT);
    app_cleanup(&a);
    a=fresh();
    write=args("must-not-exist.txt");
    put(write,"content",str("not executed"));
    put(write,"expected_sha256",nullptr);
    last=arr();
    add(last,model_call("write","write_file",write));
    J *bad=model_call("malformed","read_file",args("stable.txt"));
    put(get(bad,"function"),"arguments",str("{} trailing garbage"));
    add(last,bad);
    CHECK(workflow(&a,{model_reply(last)})!=0);
    CHECK(strstr(a.error,"JSON 对象"));
    CHECK(!path_suffix(path,a.root,"/must-not-exist.txt"));
    CHECK(access(path,F_OK)!=0&&errno==ENOENT);
    app_cleanup(&a);
    a=fresh();
    fixture(&a,"/existing.txt","keep",4);
    write=args("existing.txt");
    put(write,"content",str("changed"));
    put(write,"expected_sha256",str("stale-hash"));
    CHECK(workflow(&a,{
        model_tool("conflict","write_file",write),
        model_tool("read","read_file",args("existing.txt")),
        model_reply(nullptr,"False success")
    })!=0);
    CHECK(strstr(a.error,"未恢复"));
    CHECK(!path_suffix(path,a.root,"/existing.txt"));
    char *kept=read_bytes(path,FILE_LIMIT,nullptr);
    CHECK(kept&&!strcmp(kept,"keep"));
    free(kept);
    app_cleanup(&a);
}
static void test_native_error_flags(void){
    App a=fresh();
    J *m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str("nonzero"));
    put(m,"content",str("{\"exit_code\":7,\"output\":\"failed\"}"));
    add(a.messages,m);
    m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str("timeout"));
    put(m,"content",str("{\"exit_code\":0,\"timed_out\":true}"));
    add(a.messages,m);
    JsonPtr body(native_payload(a.config,a.messages,nullptr));
    J *messages=get(body.get(),"messages"),*content=get(at(messages,alen(messages)-1),"content");
    CHECK(alen(content)==2);
    CHECK(bget(at(content,0),"is_error"));
    CHECK(bget(at(content,1),"is_error"));
    app_cleanup(&a);
}
static void test_openai_loop(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("test","openai");
    const char *responses[]={
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"write\",\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":\\\"hello.txt\\\",\\\"content\\\":\\\"hello C\\\",\\\"expected_sha256\\\":null}\"}}]}}]}", "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"read\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"hello.txt\\\"}\"}}]}}]}", "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Created and checked.\"}}]}"
    };
    pid_t child=fake_server(&a,responses,3);
    int result=run_task(&a,"create and verify");
    server_done(child);
    CHECK(result==0);
    CHECK(!strcmp(a.state,"本轮结束"));
    J *request=capture(&a,2);
    CHECK(!strcmp(sget(request,"path"),"/v1/chat/completions"));
    CHECK(get(request,"authorization"));
    J *messages=get(get(request,"body"),"messages"),*last=at(messages,alen(messages)-1);
    CHECK(!strcmp(sget(last,"role"),"tool"));
    CHECK(strstr(sget(last,"content"),"hello C"));
    json_object_put(request);
    app_cleanup(&a);
}
static void test_anthropic_loop(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("claude","anthropic");
    const char *responses[]={
        "{\"usage\":{\"input_tokens\":5,\"cache_read_input_tokens\":7,\"cache_creation_input_tokens\":11,\"output_tokens\":3},\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"signed-value\"},{\"type\":\"tool_use\",\"id\":\"write\",\"name\":\"write_file\",\"input\":{\"path\":\"hello.txt\",\"content\":\"native C\",\"expected_sha256\":null}},{\"type\":\"tool_use\",\"id\":\"read\",\"name\":\"read_file\",\"input\":{\"path\":\"hello.txt\"}}]}", "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Native done.\"}]}"
    };
    pid_t child=fake_server(&a,responses,2);
    int result=run_task(&a,"native create and read");
    server_done(child);
    CHECK(result==0);
    JsonPtr usage(load_tokens(a));CHECK(model_tokens(usage.get(),a.config)==26);
    J *r=capture(&a,1);
    CHECK(!get(r,"authorization"));
    CHECK(get(r,"key"));
    CHECK(get(r,"version"));
    CHECK(!strcmp(sget(r,"path"),"/v1/messages"));
    J *body=get(r,"body"),*messages=get(body,"messages");
    CHECK(!get(body,"system"));
    J *previous=at(messages,alen(messages)-2);
    CHECK(!strcmp(sget(at(get(previous,"content"),0),"signature"),"signed-value"));
    J *last=at(messages,alen(messages)-1);
    CHECK(!strcmp(sget(last,"role"),"user"));
    CHECK(alen(get(last,"content"))==2);
    CHECK(!strcmp(sget(at(get(last,"content"),0),"type"),"tool_result"));
    json_object_put(r);
    app_cleanup(&a);
}
static void test_failed_task(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("test","openai");
    const char *responses[]={
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"bad\",\"function\":{\"name\":\"run_shell\",\"arguments\":\"{\\\"command\\\":\\\"exit 7\\\"}\"}}]}}]}", "{\"choices\":[{\"message\":{\"content\":\"False success\"}}]}"
    };
    pid_t child=fake_server(&a,responses,2);
    int result=run_task(&a,"bad command");
    server_done(child);
    CHECK(result!=0);
    CHECK(!strcmp(a.state,"失败"));
    CHECK(!strstr(json_object_to_json_string(a.messages),"False success"));
    app_cleanup(&a);
}
static void test_menu(void){
    App a=fresh();
    int master;
    pid_t child=forkpty(&master,NULL,NULL,NULL);
    CHECK(child>=0);
    if(!child){
        execl("./build/desk","desk","--workspace",a.root,(char *)NULL);
        _exit(127);
    }
    const char *keys="3\n0\n";
    CHECK(write(master,keys,strlen(keys))==(ssize_t)strlen(keys));
    char output[16384];
    size_t used=0;bool eof_sent=false;
    double until=monotime()+5;
    while(used+1<sizeof output&&monotime()<until){
        struct pollfd p={
            master,POLLIN,0
        };
        int ready=poll(&p,1,100);
        if(ready<0)break;
        if(ready>0){
            ssize_t n=read(master,output+used,sizeof output-used-1);
            if(n<=0)break;
            used+=(size_t)n;
            std::string text(output,used);auto first=text.find("请选择编号：");
            if(!eof_sent&&first!=std::string::npos&&text.find("请选择编号：",first+1)!=std::string::npos){CHECK(write(master,"\x04",1)==1);eof_sent=true;}
        }
    }
    output[used]=0;
    close(master);
    int status;
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(strstr(output,"[1] 开始对话"));
    CHECK(strstr(output,"[1] OpenAI接口"));
    CHECK(strstr(output,"[2] Anthropic接口"));
    CHECK(!strstr(output,"API Key"));
    app_cleanup(&a);
}
static std::string task_state(Tasks &jobs,int id){
    JsonPtr list(jobs.snapshot());
    for(size_t i=0;i<alen(list.get());i++)if(iget(at(list.get(),i),"id",0)==id)return sget(at(list.get(),i),"state");
    return "missing";
}
static bool await_state(Tasks &jobs,int id,const std::string &state,double seconds=6){
    double until=monotime()+seconds;
    while(monotime()<until){if(task_state(jobs,id)==state)return true;usleep(10000);}
    return false;
}
/* The server replies only AFTER both requests arrive: serial execution deadlocks. */
static pid_t parallel_server(App &a){
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(!bind(fd,reinterpret_cast<sockaddr *>(&address),sizeof address));
    socklen_t length=sizeof address;CHECK(!getsockname(fd,reinterpret_cast<sockaddr *>(&address),&length));CHECK(!listen(fd,4));
    put(a.config,"base_url",str(("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/v1").c_str()));
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        alarm(10);FILE *streams[2]{};std::string bodies[2];
        for(int i=0;i<2;i++){
            int client=accept(fd,nullptr,nullptr);if(client<0)_exit(2);
            streams[i]=fdopen(client,"r+");if(!streams[i])_exit(2);
            char *line=nullptr;size_t cap=0;long size=0;
            while(getline(&line,&cap,streams[i])>0&&strcmp(line,"\r\n"))if(!strncasecmp(line,"Content-Length:",15))size=strtol(line+15,nullptr,10);
            free(line);if(size<0||size>500000)_exit(2);
            bodies[i].resize(static_cast<size_t>(size));if(fread(bodies[i].data(),1,bodies[i].size(),streams[i])!=bodies[i].size())_exit(2);
        }
        if(bodies[0].find("alpha")!=std::string::npos&&bodies[0].find("beta")!=std::string::npos)_exit(3);
        if(bodies[1].find("alpha")!=std::string::npos&&bodies[1].find("beta")!=std::string::npos)_exit(3);
        std::string reply=model_reply(nullptr,"独立任务完成");
        for(FILE *stream:streams){fprintf(stream,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",reply.size(),reply.c_str());fclose(stream);}
        int client=accept(fd,nullptr,nullptr);if(client<0)_exit(2);
        FILE *stream=fdopen(client,"r+");if(!stream)_exit(2);
        char *line=nullptr;size_t cap=0;long size=0;
        while(getline(&line,&cap,stream)>0&&strcmp(line,"\r\n"))if(!strncasecmp(line,"Content-Length:",15))size=strtol(line+15,nullptr,10);
        free(line);if(size<0||size>500000)_exit(2);
        std::string continued(static_cast<size_t>(size),'\0');if(fread(continued.data(),1,continued.size(),stream)!=continued.size())_exit(2);
        if(continued.find("alpha")==std::string::npos||continued.find("followup")==std::string::npos||continued.find("beta")!=std::string::npos)_exit(3);
        fprintf(stream,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",reply.size(),reply.c_str());fclose(stream);
        close(fd);_exit(0);
    }
    close(fd);return child;
}
static void test_parallel_tasks(void){
    App a=fresh();json_object_put(a.config);a.config=config("parallel","openai");
    pid_t server=parallel_server(a);
    {
        Tasks jobs("./build/desk");
        int first=jobs.start(a,"alpha 独立任务"),second=jobs.start(a,"beta 独立任务");
        CHECK(first==1&&second==2);
        CHECK(await_state(jobs,first,"已完成"));CHECK(await_state(jobs,second,"已完成"));
        CHECK(!jobs.cancel(first));CHECK(alen(a.messages)==0);
        CHECK(jobs.resume(first,"followup 继续原任务"));CHECK(!jobs.resume(first,"任务仍在执行"));
        CHECK(await_state(jobs,first,"已完成"));
    }
    server_done(server);app_cleanup(&a);
    a=fresh();json_object_put(a.config);a.config=config("approval","openai");a.yes=false;
    J *write=args("pending.txt");put(write,"content",str("approved only"));put(write,"expected_sha256",nullptr);
    std::string response=model_tool("write","write_file",write);const char *responses[]={response.c_str()};
    server=fake_server(&a,responses,1);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"等待确认，禁止自动写入");CHECK(id==1);
        CHECK(await_state(jobs,id,"等待确认"));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/pending.txt"));CHECK(access(path,F_OK)!=0);
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));CHECK(access(path,F_OK)!=0);
    }
    server_done(server);app_cleanup(&a);
    a=fresh();json_object_put(a.config);a.config=config("cancel","openai");
    response=model_tool("run","run_shell",command("echo $$ > child.pid; sleep 30"));responses[0]=response.c_str();
    server=fake_server(&a,responses,1);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"长命令取消测试");CHECK(id==1);
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/child.pid"));
        double until=monotime()+5;while(access(path,F_OK)&&monotime()<until){jobs.refresh();usleep(10000);}
        char *pid_text=read_bytes(path,100,nullptr);CHECK(pid_text);pid_t pid=static_cast<pid_t>(strtol(pid_text,nullptr,10));free(pid_text);
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));
        until=monotime()+2;while(alive(pid)&&monotime()<until)usleep(10000);CHECK(!alive(pid));
    }
    server_done(server);app_cleanup(&a);
}
static void test_parallel_resources(void){
    App a=fresh();json_object_put(a.config);a.config=config("locks","openai");
    char other[PATH_MAX];CHECK(!path_suffix(other,a.root,"/other"));CHECK(!mkdirs(other,0700));
    J *write=args("result.txt");put(write,"content",str("independent"));put(write,"expected_sha256",nullptr);
    std::string action=model_tool("write","write_file",write),done=model_reply(nullptr,"已核对");
    const char *responses[]={action.c_str(),action.c_str(),done.c_str()};pid_t server=fake_server(&a,responses,3);
    {
        ResourceLock held(a,false);CHECK(held.acquired());
        double start=monotime();{ResourceLock blocked(a,false);CHECK(!blocked.acquired());}CHECK(monotime()-start<1);
        Tasks jobs("./build/desk");int first=jobs.start(a,"等待同目录锁");CHECK(first==1);
        CHECK(await_state(jobs,first,"等待资源"));
        int second=jobs.start(a,"在另一个目录工作",other);CHECK(second==2);
        CHECK(await_state(jobs,second,"已完成"));CHECK(task_state(jobs,first)=="等待资源");
        CHECK(jobs.cancel(first));CHECK(await_state(jobs,first,"已取消"));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/result.txt"));CHECK(access(path,F_OK)!=0);
        CHECK(!path_suffix(path,other,"/result.txt"));char *text=read_bytes(path,100,nullptr);CHECK(text&&!strcmp(text,"independent"));free(text);
    }
    server_done(server);app_cleanup(&a);
}
static std::string terminal_text(const std::string &bytes){
    std::string text;
    for(size_t i=0;i<bytes.size();){
        if(bytes[i]=='\x1b'&&i+1<bytes.size()&&bytes[i+1]=='['){i+=2;while(i<bytes.size()&&(static_cast<unsigned char>(bytes[i])<0x40||static_cast<unsigned char>(bytes[i])>0x7e))i++;if(i<bytes.size())i++;}
        else text+=bytes[i++];
    }
    return text;
}
static bool terminal_until(int fd,std::string &output,const char *needle){
    double until=monotime()+8;
    while(monotime()<until){
        if(output.find(needle)!=std::string::npos||terminal_text(output).find(needle)!=std::string::npos)return true;
        pollfd p{fd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
        char buffer[4096];ssize_t n=read(fd,buffer,sizeof buffer);if(n<=0)break;
        output.append(buffer,static_cast<size_t>(n));
    }
    fprintf(stderr,"Terminal transcript:\n%s\n",output.c_str());return false;
}
static void test_task_switch_approval(bool allow=true){
    App a=fresh();json_object_put(a.config);a.config=config("switch","openai");
    J *write=args("confirmed.txt");put(write,"content",str("user approved"));put(write,"expected_sha256",nullptr);
    std::string action=model_tool("write","write_file",write),done=model_reply(nullptr,"已核对"),continued=model_reply(nullptr,"继续检查完成");
    const char *responses[]={action.c_str(),done.c_str(),continued.c_str()};pid_t server=fake_server(&a,responses,allow?3:1);
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("DESK_BASE_URL",sget(a.config,"base_url"),1);setenv("DESK_MODEL","model-test",1);setenv("DESK_API_KEY","fake-test-key",1);setenv("DESK_API_TYPE","openai",1);
        execl("./build/desk","desk","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    const char *keys="1\n1\n创建文件\n";CHECK(::write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    CHECK(terminal_until(master,output,"[3] 查看完整操作 [0] 取消："));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/confirmed.txt"));CHECK(access(path,F_OK)!=0);
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"选择对话（仅本次运行保留）"));
    output.clear();CHECK(::write(master,"1\n",2)==2);
    CHECK(terminal_until(master,output,"[3] 查看完整操作 [0] 取消："));
    output.clear();CHECK(::write(master,allow?"1\n":"2\n",2)==2);
    CHECK(terminal_until(master,output,allow?"已核对":"用户拒绝写入"));
    if(allow){
        const char *next="再检查文件\n";CHECK(::write(master,next,strlen(next))==static_cast<ssize_t>(strlen(next)));
        CHECK(terminal_until(master,output,"继续检查完成"));
    }
    CHECK(output.find("任务管理")==std::string::npos);
    CHECK(output.find("fake-test-key")==std::string::npos);
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"选择对话（仅本次运行保留）"));
    CHECK(::write(master,"0\n",2)==2);
    CHECK(terminal_until(master,output,"[1] 开始对话"));
    CHECK(::write(master,"0\n",2)==2);
    int status=0;double until=monotime()+5;pid_t ended=0;
    while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(10000);
    if(!ended){kill(child,SIGKILL);waitpid(child,&status,0);}
    CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);close(master);
    char *text=read_bytes(path,100,nullptr);CHECK(allow?(text&&!strcmp(text,"user approved")):!text);free(text);
    CHECK(output.find("fake-test-key")==std::string::npos);
    server_done(server);app_cleanup(&a);
}
static void test_language_preferences(void){
    App a=fresh();json_object_put(a.config);a.config=config("unchanged-model","openai");
    std::string before=json_object_to_json_string_ext(a.config,JSON_C_TO_STRING_PLAIN);
    std::string store=json_object_to_json_string_ext(a.store,JSON_C_TO_STRING_PLAIN);
    J *user=obj();put(user,"role",str("user"));put(user,"content",str("Keep my request exactly."));add(a.messages,user);
    CHECK(save_language(a,"en"));CHECK(!strcmp(language_code(),"en"));
    CHECK(!strcmp(tr("等待任务"),"Waiting for a task"));
    CHECK(before==json_object_to_json_string_ext(a.config,JSON_C_TO_STRING_PLAIN));
    CHECK(store==json_object_to_json_string_ext(a.store,JSON_C_TO_STRING_PLAIN));
    CHECK(alen(a.messages)==1&&!strcmp(sget(at(a.messages,0),"content"),"Keep my request exactly."));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/ui.json"));
    char *text=read_bytes(path,4096,nullptr);CHECK(text);JsonPtr pref(json_tokener_parse(text));free(text);
    CHECK(json_object_object_length(pref.get())==1);CHECK(!strcmp(sget(pref.get(),"language"),"en"));
    CHECK(!path_suffix(path,a.config_dir,"/settings.json"));CHECK(access(path,F_OK)!=0);
    unsetenv("DESK_LANG");char program[]="desk";char *args[]={program,nullptr};
    CHECK(set_language("zh"));CHECK(language_init(1,args));CHECK(!strcmp(language_code(),"en"));
    CHECK(!setenv("DESK_LANG","zh",1));CHECK(language_init(1,args));CHECK(!strcmp(language_code(),"zh"));
    char flag[]="--lang",english[]="en",help[]="--help";char *override_args[]={program,help,flag,english,nullptr};
    CHECK(language_init(4,override_args));CHECK(!strcmp(language_code(),"en"));
    unsetenv("DESK_LANG");CHECK(save_language(a,"zh"));CHECK(!strcmp(language_code(),"zh"));
    app_cleanup(&a);
}
static void test_english_menu(void){
    App a=fresh();int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(terminal_until(master,output,"[3] Add model"));CHECK(output.find("[2] Select model")!=std::string::npos);
    CHECK(write(master,"3\n",2)==2);CHECK(terminal_until(master,output,"[1] OpenAI API"));
    CHECK(terminal_until(master,output,"[2] Anthropic API"));
    const char *quit="0\n4\n1\n1\n0\n\x04";CHECK(write(master,quit,strlen(quit))==static_cast<ssize_t>(strlen(quit)));
    CHECK(terminal_until(master,output,"[1] 开始对话"));
    int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    app_cleanup(&a);
}
static void test_no_extra_model_prompts(void){
    App a=fresh();CHECK(set_language("en"));
    CHECK(!workflow(&a,{model_reply(nullptr,"Provider reply, unchanged.")}));
    JsonPtr request(capture(&a,0));J *body=get(request.get(),"body"),*messages=get(body,"messages");
    CHECK(!strcmp(sget(body,"model"),"model-test"));CHECK(alen(messages)==1);
    CHECK(!strcmp(sget(at(messages,0),"role"),"user"));CHECK(!strcmp(sget(at(messages,0),"content"),"完成任务并检查实际结果"));
    CHECK(!get(body,"system"));CHECK(!strcmp(sget(at(a.messages,alen(a.messages)-1),"content"),"Provider reply, unchanged."));
    CHECK(set_language("zh"));app_cleanup(&a);
}
static void wait_for_chat_prompt(int master,const std::string &output,const char *after){
    auto offset=output.rfind(after);CHECK(offset!=std::string::npos);
    auto tail=output.substr(offset+strlen(after));
    CHECK(terminal_until(master,tail,"\r\n: "));
}
static void leave_chat_and_exit(int master,bool english=true){
    CHECK(write(master,"\x04",1)==1);
    const char *picker=english?"Choose a conversation (this run only)":"选择对话（仅本次运行保留）";
    std::string output;CHECK(terminal_until(master,output,picker));
    output.erase(0,output.find(picker));
    CHECK(terminal_until(master,output,english?"Select a number:":"请选择编号："));
    output.clear();
    CHECK(write(master,"0\n",2)==2);
    CHECK(terminal_until(master,output,english?"[1] Start chat":"[1] 开始对话"));
    CHECK(write(master,"0\n",2)==2);
}
static void test_numeric_request(void){
    App a=fresh();json_object_put(a.config);a.config=config("numeric","openai");
    std::string reply=model_reply(nullptr,"数字需求已收到");std::string zero=model_reply(nullptr,"零也是需求");const char *responses[]={reply.c_str(),zero.c_str()};pid_t server=fake_server(&a,responses,2);
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("DESK_BASE_URL",sget(a.config,"base_url"),1);setenv("DESK_MODEL","model-test",1);setenv("DESK_API_KEY","fake-test-key",1);setenv("DESK_API_TYPE","openai",1);
        execl("./build/desk","desk","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    const char *keys="1\n1\n1\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    std::string output;CHECK(terminal_until(master,output,"数字需求已收到"));
    CHECK(write(master,"0\n",2)==2);CHECK(terminal_until(master,output,"零也是需求"));
    wait_for_chat_prompt(master,output,"零也是需求");leave_chat_and_exit(master,false);int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    server_done(server);JsonPtr request(capture(&a,0));J *messages=get(get(request.get(),"body"),"messages");
    CHECK(alen(messages)==1&&!strcmp(sget(at(messages,0),"role"),"user"));CHECK(!strcmp(sget(at(messages,0),"content"),"1"));
    CHECK(output.find("[y/N]")==std::string::npos&&output.find("/help")==std::string::npos);
    JsonPtr next(capture(&a,1));J *history=get(get(next.get(),"body"),"messages");CHECK(!strcmp(sget(at(history,alen(history)-1),"content"),"0"));
    app_cleanup(&a);
}
static void test_numeric_settings(void){
    App a=fresh();int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    const char *keys="4\n2\n1\n3\n2\n0\n0\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    std::string output;CHECK(terminal_until(master,output,"Approve file edits (this run)"));
    CHECK(output.find("[6] View status")==std::string::npos);CHECK(output.find("[4] Settings")!=std::string::npos);CHECK(output.find("[4] Tasks")==std::string::npos);
    int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(output.find("/desktop")==std::string::npos&&output.find("/permissions")==std::string::npos);
    CHECK(output.find("\n  Task:")==std::string::npos&&output.find("\n  Desktop:")==std::string::npos);
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/settings.json"));CHECK(access(path,F_OK)!=0);
    app_cleanup(&a);
}
static void test_approval_modes(void){
    App a=fresh();a.yes=false;
    CHECK(approval_required(a,Operation::FileEdit));CHECK(approval_required(a));
    a.auto_edit=true;CHECK(!approval_required(a,Operation::FileEdit));CHECK(approval_required(a));
    JsonPtr write(args("automatic.txt"));put(write.get(),"content",str("first"));put(write.get(),"expected_sha256",nullptr);
    JsonPtr result(tool_call(&a,"write_file",write.get()));CHECK(bget(result.get(),"verified"));
    JsonPtr path(args("automatic.txt"));result.reset(tool_call(&a,"read_file",path.get()));std::string hash=sget(result.get(),"sha256");
    put(path.get(),"old_text",str("first"));put(path.get(),"new_text",str("second"));put(path.get(),"expected_sha256",str(hash.c_str()));
    result.reset(tool_call(&a,"edit_file",path.get()));CHECK(bget(result.get(),"verified"));
    put(write.get(),"path",str("../outside.txt"));result.reset(tool_call(&a,"write_file",write.get()));CHECK(get(result.get(),"error"));
    // A redirected foreground cannot approve commands or desktop actions in assisted mode.
    int saved=dup(STDIN_FILENO),null=open("/dev/null",O_RDONLY);CHECK(saved>=0&&null>=0);CHECK(dup2(null,STDIN_FILENO)>=0);close(null);
    JsonPtr cmd(command("touch forbidden.txt"));result.reset(tool_call(&a,"run_shell",cmd.get()));CHECK(bget(result.get(),"denied"));
    a.desktop=true;JsonPtr desktop(obj());put(desktop.get(),"ref",str("invalid"));put(desktop.get(),"action",str("invoke"));result.reset(tool_call(&a,"desktop_action",desktop.get()));CHECK(bget(result.get(),"denied"));
    CHECK(dup2(saved,STDIN_FILENO)>=0);close(saved);
    a.yes=true;CHECK(!approval_required(a));CHECK(!approval_required(a,Operation::FileEdit));
    CHECK(set_language("en"));CHECK(!strcmp(permission_name(a),"Full access"));a.yes=false;CHECK(!strcmp(permission_name(a),"Approve file edits"));a.auto_edit=false;CHECK(!strcmp(permission_name(a),"Ask for approval"));CHECK(set_language("zh"));
    app_cleanup(&a);
}
static void test_reasoning_modes(void){
    for(bool native:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("reasoning",native?"anthropic":"openai");
        J *user=obj();put(user,"role",str("user"));put(user,"content",str("Keep this exact request."));add(a.messages,user);
        const char *openai[]={"none","minimal","low","medium","high","xhigh"};
        const char *anthropic[]={"none","low","medium","high","xhigh","max"};const char **levels=native?anthropic:openai;
        std::string reply=native?"{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"unchanged\"}]}":model_reply(nullptr,"unchanged");
        std::vector<const char *> responses(8,reply.c_str());pid_t server=fake_server(&a,responses.data(),static_cast<int>(responses.size()));
        std::string before=json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN),model=sget(a.config,"model");int tokens=iget(a.config,"max_tokens",4096);
        for(int i=0;i<8;i++){
            JsonPtr candidate(copy(a.config));if(i==0||i==7)json_object_object_del(candidate.get(),"reasoning_effort");else put(candidate.get(),"reasoning_effort",str(levels[i-1]));
            CHECK(!save_config(&a,candidate.get(),false));CHECK(before==json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN));CHECK(model==sget(a.config,"model"));CHECK(tokens==iget(a.config,"max_tokens",4096));
            App reload;CHECK(!store_load(&reload));CHECK(!strcmp(sget(reload.config,"reasoning_effort"),sget(a.config,"reasoning_effort")));app_cleanup(&reload);
            JsonPtr result(api_complete(&a,a.config,a.messages,nullptr));CHECK(result);CHECK(!strcmp(sget(result.get(),"content"),"unchanged"));
            JsonPtr request(capture(&a,i));J *body=get(request.get(),"body");CHECK(!get(body,"system"));CHECK(model==sget(body,"model"));CHECK(alen(get(body,"messages"))==1);
            if(i==0||i==7){CHECK(!get(body,"reasoning_effort"));CHECK(!get(body,"thinking"));CHECK(!get(body,"output_config"));}
            else if(!native)CHECK(!strcmp(sget(body,"reasoning_effort"),levels[i-1]));
            else if(i==1){CHECK(!strcmp(sget(get(body,"thinking"),"type"),"disabled"));CHECK(!get(body,"output_config"));}
            else{CHECK(!strcmp(sget(get(body,"thinking"),"type"),"adaptive"));CHECK(!strcmp(sget(get(body,"output_config"),"effort"),levels[i-1]));CHECK(iget(body,"max_tokens",0)==tokens);}
        }
        server_done(server);
        put(a.config,"reasoning_effort",str("xhigh"));const char *rejected[]={"{\"error\":{\"message\":\"unsupported effort\"}}"};server=fake_server(&a,rejected,1,400);
        JsonPtr failure(api_complete(&a,a.config,a.messages,nullptr));CHECK(!failure);CHECK(strstr(a.error,"400"));CHECK(strstr(a.error,"推理"));CHECK(!strcmp(sget(a.config,"reasoning_effort"),"xhigh"));CHECK(model==sget(a.config,"model"));CHECK(before==json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN));server_done(server);
        put(a.config,"reasoning_effort",str("invalid"));CHECK(config_valid(&a,a.config));put(a.config,"reasoning_effort",json_object_new_int(5));CHECK(config_valid(&a,a.config));
        put(a.config,"reasoning_effort",str("minimal"));CHECK(native?config_valid(&a,a.config)!=0:config_valid(&a,a.config)==0);
        app_cleanup(&a);
    }
}
static void test_assisted_worker(void){
    App a=fresh();json_object_put(a.config);a.config=config("assisted","openai");a.yes=false;a.auto_edit=true;put(a.config,"reasoning_effort",str("high"));
    J *write=args("assisted.txt");put(write,"content",str("auto-approved"));put(write,"expected_sha256",nullptr);
    std::string first=model_tool("write","write_file",write),second=model_tool("run","run_shell",command("touch command-must-wait.txt"));first.insert(first.size()-1,",\"usage\":{\"total_tokens\":17}");
    second.insert(second.size()-1,",\"usage\":{\"prompt_tokens\":19,\"completion_tokens\":4}");
    const char *responses[]={first.c_str(),second.c_str()};pid_t server=fake_server(&a,responses,2);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"edit then ask for command approval");CHECK(id==1);
        a.auto_edit=false;a.yes=true;put(a.config,"reasoning_effort",str("low"));
        CHECK(await_state(jobs,id,"等待确认"));JsonPtr usage(load_tokens(a));CHECK(model_tokens(usage.get(),a.config)==40);char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/assisted.txt"));char *content=read_bytes(path,100,nullptr);CHECK(content&&!strcmp(content,"auto-approved"));free(content);
        CHECK(!path_suffix(path,a.root,"/command-must-wait.txt"));CHECK(access(path,F_OK)!=0);
        JsonPtr request(capture(&a,1));CHECK(!strcmp(sget(get(request.get(),"body"),"reasoning_effort"),"high"));
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));CHECK(access(path,F_OK)!=0);
    }
    server_done(server);app_cleanup(&a);
}
static void test_chat_mode_menu(void){
    App a=fresh();json_object_put(a.config);a.config=config("modes","openai");CHECK(!save_config(&a,a.config));
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    const char *keys="1\n1\n/\n5\n5\n/\n6\n2\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));std::string output;
    CHECK(terminal_until(master,output,"Reasoning: High · Approval: Approve file edits"));CHECK(output.find("[6] Extra high (xhigh)")!=std::string::npos);CHECK(output.find("[3] Full access")!=std::string::npos);CHECK(output.find("[1] Enter a request")==std::string::npos);
    const char *reset="/\n5\n7\n/\n6\n3\n";CHECK(write(master,reset,strlen(reset))==static_cast<ssize_t>(strlen(reset)));CHECK(terminal_until(master,output,"Reasoning: Provider default (unspecified) · Approval: Full access"));
    const char *ask="/\n6\n1\n";CHECK(write(master,ask,strlen(ask))==static_cast<ssize_t>(strlen(ask)));CHECK(terminal_until(master,output,"Reasoning: Provider default (unspecified) · Approval: Ask for approval"));
    wait_for_chat_prompt(master,output,"Reasoning: Provider default (unspecified) · Approval: Ask for approval");leave_chat_and_exit(master);int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    app_cleanup(&a);
}
static void test_chat_exit_paths(void){
    for(int route=0;route<5;route++){
        App a=fresh();json_object_put(a.config);a.config=config("exit","openai");put(a.config,"base_url",str("http://127.0.0.1:1/v1"));CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        std::string output;
        if(route==4){CHECK(terminal_until(master,output,"[0] Exit"));}
        else{
            CHECK(write(master,"1\n1\n",4)==4);CHECK(terminal_until(master,output,"Approval: Ask for approval\r\n: "));output.clear();
            if(route==1){CHECK(write(master,"\n",1)==1);CHECK(terminal_until(master,output,": "));CHECK(output.find("Approval:")==std::string::npos);}
            else if(route==2){CHECK(kill(child,SIGINT)==0);CHECK(terminal_until(master,output,": "));CHECK(output.find("Approval:")==std::string::npos);}
            else if(route==3){CHECK(write(master,"/\n5\n",4)==4);CHECK(terminal_until(master,output,"[7] Restore provider default"));}
        }
        CHECK(kill(child,0)==0);
        if(route==4){CHECK(terminal_until(master,output,"Select a number:"));CHECK(write(master,"0\n",2)==2);}
        else{if(route==3){output.erase(0,output.find("[7] Restore provider default"));CHECK(terminal_until(master,output,"Select a number:"));}leave_chat_and_exit(master);}
        int status=0;pid_t ended=0;double until=monotime()+5;
        while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(10000);
        if(!ended){kill(child,SIGKILL);waitpid(child,&status,0);}
        close(master);CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);CHECK(output.find("Thinking...")==std::string::npos);app_cleanup(&a);
    }
}
static void test_compact_output(void){
    CHECK(set_language("zh"));
    for(bool verbose:{false,true}){
        App a=fresh();a.verbose=verbose;
        J *write=args("private-content.txt");put(write,"content",str("hidden-file-contents"));put(write,"expected_sha256",nullptr);
        J *calls=arr();add(calls,model_call("write","write_file",write));
        std::vector<std::string> replies={model_reply(calls,"hidden-intermediate-comment"),model_tool("run","run_shell",command("printf 'hidden-command-output'")),model_reply(nullptr,"Final reply, unchanged.")};
        fflush(stdout);FILE *log=tmpfile();CHECK(log);int saved=dup(STDOUT_FILENO);CHECK(saved>=0);CHECK(dup2(fileno(log),STDOUT_FILENO)>=0);
        int rc=workflow(&a,replies);fflush(stdout);CHECK(dup2(saved,STDOUT_FILENO)>=0);close(saved);
        long size=ftell(log);CHECK(size>=0);rewind(log);std::string output(static_cast<size_t>(size),'\0');CHECK(fread(output.data(),1,output.size(),log)==output.size());fclose(log);CHECK(!rc);
        CHECK(output.find("Final reply, unchanged.")!=std::string::npos);
        CHECK((output.find("hidden-file-contents")!=std::string::npos)==verbose);
        CHECK((output.find("hidden-intermediate-comment")!=std::string::npos)==verbose);
        CHECK((output.find("hidden-command-output")!=std::string::npos)==verbose);
        CHECK((output.find("正在思考…")!=std::string::npos)==!verbose);
        std::string history=json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN);CHECK(history.find("hidden-command-output")!=std::string::npos);CHECK(history.find("hidden-intermediate-comment")!=std::string::npos);
        app_cleanup(&a);
    }
}
static void test_exit_during_work(void){
    for(bool command_running:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("exit-active","openai");
        std::string reply=command_running?model_tool("run","run_shell",command("echo $$ > running.pid; sleep 30")):model_reply(nullptr,"must not wait for this reply");const char *responses[]={reply.c_str()};pid_t server=fake_server(&a,responses,1,200,command_running?0:8000);CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        const char *keys="1\n1\nwork\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
        std::string output;CHECK(terminal_until(master,output,command_running?"Running a command...":"Thinking..."));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,command_running?"/running.pid":"/request-0.json"));double until=monotime()+5;while(access(path,F_OK)&&monotime()<until)usleep(10000);CHECK(access(path,F_OK)==0);
        pid_t process=-1;if(command_running){char *id=read_bytes(path,100,nullptr);CHECK(id);process=static_cast<pid_t>(strtol(id,nullptr,10));free(id);}
        double exit_started=monotime();leave_chat_and_exit(master);CHECK(monotime()-exit_started<4);int status=0;pid_t ended=0;until=monotime()+4;
        while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(10000);
        if(!ended){kill(child,SIGKILL);waitpid(child,&status,0);}
        close(master);
        if(command_running){server_done(server);until=monotime()+2;while(alive(process)&&monotime()<until)usleep(10000);CHECK(!alive(process));}
        else{kill(server,SIGKILL);waitpid(server,nullptr,0);}
        CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);app_cleanup(&a);
    }
}
static void test_compact_approval(void){
    for(bool expand:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("approval-preview","openai");
        J *write=args("review.txt");put(write,"content",str("hidden-preview-body"));put(write,"expected_sha256",nullptr);
        std::string reply=model_tool("write","write_file",write);const char *responses[]={reply.c_str()};pid_t server=fake_server(&a,responses,1);CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        const char *keys="1\n1\ncreate a file\n";CHECK(::write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));std::string output;
        CHECK(terminal_until(master,output,"[3] Show full operation [0] Cancel: "));CHECK(output.find("hidden-preview-body")==std::string::npos);CHECK(output.find("review.txt")!=std::string::npos);
        if(expand){CHECK(::write(master,"3\n",2)==2);CHECK(terminal_until(master,output,"hidden-preview-body"));CHECK(::write(master,"2\n",2)==2);CHECK(terminal_until(master,output,"Operation cancelled."));wait_for_chat_prompt(master,output,"Operation cancelled.");}
        leave_chat_and_exit(master);int status=0;pid_t ended=0;double until=monotime()+5;
        while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(10000);
        if(!ended){kill(child,SIGKILL);waitpid(child,&status,0);}
        close(master);CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/review.txt"));CHECK(access(path,F_OK)!=0);server_done(server);app_cleanup(&a);
    }
}
static void type_terminal(int fd,std::string &output,const std::string &text){
    size_t used=0;double until=monotime()+12;
    while(used<text.size()&&monotime()<until){
        struct pollfd ready{fd,POLLIN|POLLOUT,0};CHECK(poll(&ready,1,100)>=0);
        if(ready.revents&POLLIN){char bytes[8192];ssize_t n=read(fd,bytes,sizeof bytes);CHECK(n>0);output.append(bytes,static_cast<size_t>(n));}
        if(ready.revents&POLLOUT){size_t count=std::min(static_cast<size_t>(128),text.size()-used);ssize_t n=write(fd,text.data()+used,count);CHECK(n>0);used+=static_cast<size_t>(n);}
    }
    CHECK(used==text.size());
}
static void test_line_editing(void){
    App a=fresh();json_object_put(a.config);a.config=config("line-edit","openai");
    std::vector<std::pair<std::string,std::string>> cases={
        {"你好😀"+std::string(3,'\x7f')+"hi","hi"},
        {"ab中cd\x1b[D\x1b[D\x7f\x1b[3~文\x05\x7f\x01首","首ab文"},
        {std::string(400,'x')+"测试😀"+std::string(403,'\x7f')+"erase-ok","erase-ok"},
        {std::string(5000,'z'),std::string(5000,'z')},
        {std::string(5000,'z')+std::string(5000,'\x7f')+"clean","clean"},
        {"\x1b[200~多行\n内容\x1b[201~","多行\n内容"}
    };
    std::vector<std::string> replies;std::vector<const char *> responses;
    for(size_t i=0;i<cases.size();i++)replies.push_back(model_reply(nullptr,("EDIT_DONE_"+std::to_string(i)).c_str()));
    for(auto &reply:replies)responses.push_back(reply.c_str());
    pid_t server=fake_server(&a,responses.data(),static_cast<int>(responses.size()));CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{12,24,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);setenv("INPUTRC","/dev/null",1);struct termios mode{};if(!tcgetattr(STDIN_FILENO,&mode)){mode.c_iflag&=~IUTF8;tcsetattr(STDIN_FILENO,TCSANOW,&mode);}
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;CHECK(write(master,"1\n1\n",4)==4);CHECK(terminal_until(master,output,"Approval: Ask for approval\r\n: "));
    for(size_t i=0;i<cases.size();i++){
        output.clear();type_terminal(master,output,cases[i].first+"\n");std::string done="EDIT_DONE_"+std::to_string(i);CHECK(terminal_until(master,output,done.c_str()));wait_for_chat_prompt(master,output,done.c_str());
        JsonPtr request(capture(&a,static_cast<int>(i)));J *messages=get(get(request.get(),"body"),"messages");CHECK(cases[i].second==sget(at(messages,alen(messages)-1),"content"));CHECK(valid_utf8(sget(at(messages,alen(messages)-1),"content")));
    }
    leave_chat_and_exit(master);int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);server_done(server);app_cleanup(&a);
}
static void test_queued_editing(void){
    App a=fresh();json_object_put(a.config);a.config=config("queued-edit","openai");
    std::string first=model_tool("run","run_shell",command("sleep .4")),second=model_reply(nullptr,"PRETYPE_DONE"),third=model_reply(nullptr,"QUEUED_DONE");const char *responses[]={first.c_str(),second.c_str(),third.c_str()};pid_t server=fake_server(&a,responses,3);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{24,80,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);setenv("INPUTRC","/dev/null",1);execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    CHECK(write(master,"1\n1\nfirst\n",10)==10);std::string output;CHECK(terminal_until(master,output,"Running a command..."));
    std::string queued="排队😀\x7f下一句\n";type_terminal(master,output,queued);CHECK(terminal_until(master,output,"QUEUED_DONE"));wait_for_chat_prompt(master,output,"QUEUED_DONE");
    JsonPtr request(capture(&a,2));J *messages=get(get(request.get(),"body"),"messages");CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"排队下一句"));
    leave_chat_and_exit(master);int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);server_done(server);app_cleanup(&a);
}
static void test_conversation_picker(void){
    App a=fresh();json_object_put(a.config);a.config=config("conversation-picker","openai");
    const std::string first="请帮我整理比赛资料。后续说明",second="Draft a demo video",follow="Continue the notes",third="Write a release checklist";
    std::vector<std::string> replies={model_reply(nullptr,"REPLY_ONE"),model_reply(nullptr,"REPLY_TWO"),model_reply(nullptr,"REPLY_RESUMED"),model_reply(nullptr,"REPLY_FRESH")};
    replies[0].insert(replies[0].size()-1,",\"usage\":{\"total_tokens\":12}");
    replies[1].insert(replies[1].size()-1,",\"usage\":{\"total_tokens\":18}");
    const char *responses[]={replies[0].c_str(),replies[1].c_str(),replies[2].c_str(),replies[3].c_str()};
    pid_t server=fake_server(&a,responses,4);CHECK(!save_config(&a,a.config));
    JsonPtr alternate(copy(a.config));put(alternate.get(),"profile",str("alternate"));put(alternate.get(),"model",str("alternate-model"));
    CHECK(!save_config(&a,alternate.get()));CHECK(!activate(&a,"conversation-picker"));
    char state_path[PATH_MAX];CHECK(!path_suffix(state_path,a.root,"/private-state"));
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("XDG_STATE_HOME",state_path,1);
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("Imported models")!=std::string::npos);
    CHECK(output.find("2. alternate-model · 0 Token")!=std::string::npos);
    CHECK(output.find("1. model-test · 0 Token · Current")!=std::string::npos);
    type_terminal(master,output,"1\n1\n"+first+"\n");CHECK(terminal_until(master,output,"REPLY_ONE"));wait_for_chat_prompt(master,output,"REPLY_ONE");
    CHECK(write(master,"\x04",1)==1);output.clear();
    CHECK(terminal_until(master,output,"1. 整理比赛资料"));CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"3\n2\n1\n我的比赛笔记\n");
    CHECK(terminal_until(master,output,"1. 我的比赛笔记"));
    output.clear();type_terminal(master,output,"0\n2\n3\n2\n1\n2\n1\n1\n2\n"+second+"\n");CHECK(terminal_until(master,output,"REPLY_TWO"));wait_for_chat_prompt(master,output,"REPLY_TWO");
    CHECK(write(master,"\x04",1)==1);output.clear();
    CHECK(terminal_until(master,output,"1. Draft a demo video"));
    CHECK(terminal_until(master,output,"[4] Rename conversation"));
    CHECK(output.find("1. Draft a demo video")<output.find("2. 我的比赛笔记"));
    CHECK(output.find("2. 我的比赛笔记")<output.find("[4] Rename conversation"));
    output.clear();type_terminal(master,output,"5\n1\n0\n");
    CHECK(terminal_until(master,output,"[1] Confirm deletion"));
    CHECK(terminal_until(master,output,"1. Draft a demo video"));
    output.clear();type_terminal(master,output,"5\n1\n1\n");
    CHECK(terminal_until(master,output,"Choose a conversation (this run only)"));
    auto after_delete=output.substr(output.find("Choose a conversation (this run only)"));
    CHECK(terminal_until(master,after_delete,"Select a number:"));
    CHECK(after_delete.find("Draft a demo video")==std::string::npos);
    CHECK(after_delete.find("1. 我的比赛笔记")!=std::string::npos);
    output.clear();type_terminal(master,output,"1\n"+follow+"\n");
    CHECK(terminal_until(master,output,"Conversation: 我的比赛笔记"));CHECK(terminal_until(master,output,"REPLY_RESUMED"));
    output.clear();type_terminal(master,output,"/\n1\n"+third+"\n");CHECK(terminal_until(master,output,"REPLY_FRESH"));wait_for_chat_prompt(master,output,"REPLY_FRESH");
    leave_chat_and_exit(master);
    int status=0;CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    server_done(server);CHECK(access(state_path,F_OK)!=0);
    for(int i=0;i<4;i++){
        JsonPtr request(capture(&a,i));J *body=get(request.get(),"body"),*messages=get(body,"messages");
        CHECK(!get(body,"system"));CHECK(alen(messages)==(i==2?3u:1u));
        CHECK(!strcmp(sget(body,"model"),i==1?"alternate-model":"model-test"));
        const std::string expected[]={first,second,follow,third};
        CHECK(expected[i]==sget(at(messages,alen(messages)-1),"content"));
        if(i==2){CHECK(first==sget(at(messages,0),"content"));CHECK(!strcmp(sget(at(messages,1),"content"),"REPLY_ONE"));}
    }
    child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    output.clear();
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("1. alternate-model · 18 Token")!=std::string::npos);
    CHECK(output.find("2. model-test · 12 Token")!=std::string::npos);
    CHECK(write(master,"1\n",2)==2);
    CHECK(terminal_until(master,output,"Choose a conversation (this run only)"));
    auto tail=output.substr(output.find("Choose a conversation (this run only)"));
    CHECK(terminal_until(master,tail,"Select a number:"));CHECK(tail.find("我的比赛笔记")==std::string::npos);
    CHECK(write(master,"\x04",1)==1);CHECK(waitpid(child,&status,0)==child);close(master);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    app_cleanup(&a);
}

static void test_thinking_timer(){
    // Exercise the shared foreground renderer and the background chat header.
    for(bool interactive:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("thinking-timer","openai");
        std::string path=std::string(a.root)+"/note.txt";CHECK(!atomic_file(path.c_str(),"hello",5,0600));
        const auto first=interactive?model_reply(nullptr,"TIMER_FIRST_DONE"):model_tool("read","read_file",args("note.txt"));
        const auto second=model_reply(nullptr,"TIMER_SECOND_DONE");
        const char *responses[]={first.c_str(),second.c_str()};
        pid_t server=fake_server(&a,responses,2,200,2800);CHECK(!save_config(&a,a.config));
        int master=-1;struct winsize window{40,110,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
        if(!child){
            setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);
            if(interactive)execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
            else execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,"First timed request",static_cast<char *>(nullptr));
            _exit(127);
        }
        std::string output;
        if(interactive){
            CHECK(terminal_until(master,output,"Select a number:"));
            CHECK(output.find("    [1] Start chat\r\n\r\n    [2] Select model")!=std::string::npos);
            CHECK(output.find("Conversation activity")==std::string::npos);
            type_terminal(master,output,"1\n1\nFirst timed request\n");
        }
        CHECK(terminal_until(master,output,"Thinking... 0s"));
        CHECK(terminal_until(master,output,"Thinking... 1s"));
        if(interactive){
            type_terminal(master,output,"secondx\x7f");
            CHECK(terminal_until(master,output,"Thinking... 2s"));
            CHECK(terminal_until(master,output,"TIMER_FIRST_DONE"));
            output.clear();type_terminal(master,output,"\n");
        }else{
            CHECK(terminal_until(master,output,"Reading files..."));
            output.erase(0,output.find("Reading files..."));
        }
        CHECK(terminal_until(master,output,"Thinking... 0s"));
        CHECK(terminal_until(master,output,"Thinking... 1s"));
        CHECK(terminal_until(master,output,"TIMER_SECOND_DONE"));
        if(interactive){wait_for_chat_prompt(master,output,"TIMER_SECOND_DONE");leave_chat_and_exit(master);}
        int status=0;CHECK(waitpid(child,&status,0)==child);close(master);
        CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);server_done(server);
        JsonPtr request(capture(&a,1));J *messages=get(get(request.get(),"body"),"messages");
        CHECK(!strcmp(sget(at(messages,0),"content"),"First timed request"));
        if(interactive)CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"second"));
        CHECK(std::string(json_object_to_json_string(messages)).find("Thinking...")==std::string::npos);
        app_cleanup(&a);
    }
}
static void test_background_conversations(){
    App a=fresh();json_object_put(a.config);a.config=config("background-conversations","openai");
    const std::string first=model_reply(nullptr,"FIRST_DONE"),second=model_reply(nullptr,"SECOND_DONE"),follow=model_reply(nullptr,"FOLLOW_DONE");
    const char *responses[]={first.c_str(),second.c_str(),follow.c_str()};
    pid_t server=fake_server(&a,responses,3,200,1000);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{24,80,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("Conversation activity")==std::string::npos);
    CHECK(output.find("\033[?1049h")!=std::string::npos);
    type_terminal(master,output,"1\n1\nFirst conversation\n");
    CHECK(terminal_until(master,output,"Thinking..."));
    CHECK(terminal_until(master,output,"Running · First conversation"));
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"[2] New conversation"));
    output.clear();type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(kill(child,0)==0);
    CHECK(output.find("Conversation activity")==std::string::npos);
    output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"[2] New conversation"));
    output.clear();type_terminal(master,output,"2\nSecond conversation\n");
    CHECK(terminal_until(master,output,"Running · First conversation"));
    CHECK(terminal_until(master,output,"Running · Second conversation"));
    CHECK(terminal_until(master,output,"SECOND_DONE"));
    CHECK(output.find("FIRST_DONE")==std::string::npos);
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"2. First conversation"));
    type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"FIRST_DONE"));
    type_terminal(master,output,"Continue first\n");
    CHECK(terminal_until(master,output,"FOLLOW_DONE"));
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"[0] Main menu"));
    output.clear();type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("\033[?1049l")==std::string::npos);
    CHECK(output.find("Conversation activity")==std::string::npos);
    CHECK(write(master,"0\n",2)==2);
    int status=0;CHECK(waitpid(child,&status,0)==child);close(master);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);server_done(server);
    for(int i=0;i<3;i++){
        JsonPtr request(capture(&a,i));J *messages=get(get(request.get(),"body"),"messages");
        CHECK(alen(messages)==(i==2?3u:1u));
        CHECK(!strcmp(sget(at(messages,0),"content"),i==1?"Second conversation":"First conversation"));
    }
    app_cleanup(&a);
}
static int mcp_fixture(){
    char *line=nullptr;size_t capacity=0;
    while(getline(&line,&capacity,stdin)>0){
        JsonPtr message(json_tokener_parse(line));const char *method=sget(message.get(),"method");
        if(!*method||!get(message.get(),"id"))continue;
        JsonPtr response(obj());put(response.get(),"jsonrpc",str("2.0"));put(response.get(),"id",copy(get(message.get(),"id")));
        J *result=obj();
        if(!strcmp(method,"initialize")){
            puts("{\"jsonrpc\":\"2.0\",\"id\":\"server-ping\",\"method\":\"ping\"}");
            put(result,"protocolVersion",str("2025-06-18"));J *capabilities=obj();put(capabilities,"tools",obj());put(result,"capabilities",capabilities);
        }else if(!strcmp(method,"tools/list")){
            bool second=*sget(get(message.get(),"params"),"cursor");
            J *tools=arr(),*tool=obj();put(tool,"name",str(second?"fail":"echo"));
            put(tool,"description",str("MCP fixture"));
            J *schema=json_tokener_parse("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"object\"}},\"required\":[\"value\"]}");
            put(tool,"inputSchema",schema);add(tools,tool);put(result,"tools",tools);
            if(!second)put(result,"nextCursor",str("next"));
        }else if(!strcmp(method,"tools/call")){
            J *params=get(message.get(),"params");
            put(result,"isError",json_object_new_boolean(!strcmp(sget(params,"name"),"fail")));
            put(result,"structuredContent",copy(get(params,"arguments")));
            put(result,"pid",json_object_new_int(getpid()));
            put(result,"inheritedSecret",json_object_new_boolean(getenv("MCP_TEST_SECRET")!=nullptr));
            put(result,"explicitEnv",str(getenv("EXPLICIT_VALUE")));
            J *content=arr(),*item=obj();put(item,"type",str("text"));put(item,"text",str("MCP_ECHO_OK"));add(content,item);put(result,"content",content);
        }else{put(result,"unexpected",str(method));}
        put(response.get(),"result",result);puts(json_object_to_json_string_ext(response.get(),JSON_C_TO_STRING_PLAIN));fflush(stdout);
    }
    free(line);return 0;
}
static void test_mcp(){
    App a=fresh();JsonPtr servers(obj()),entry(obj());
    put(entry.get(),"command",str(path_text(fs::read_symlink("/proc/self/exe")).c_str()));
    J *arguments=arr();add(arguments,str("--mcp-fixture"));put(entry.get(),"args",arguments);
    J *environment=obj();put(environment,"EXPLICIT_VALUE",str("explicit-test-value"));put(entry.get(),"env",environment);
    put(servers.get(),"fixture",entry.release());CHECK(mcp_save(a,servers.get()));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/mcp.json"));struct stat mode{};CHECK(!stat(path,&mode));CHECK((mode.st_mode&0777)==0600);
    JsonPtr invalid(copy(servers.get()));put(get(invalid.get(),"fixture"),"disabled",str("false"));CHECK(!mcp_save(a,invalid.get()));
    JsonPtr loaded(mcp_load(a));CHECK(json_object_equal(loaded.get(),servers.get()));
    CHECK(!path_suffix(path,a.root,"/import.json"));const char *duplicate="{\"mcpServers\":{\"fixture\":{\"command\":\"bad\"}}}";CHECK(!atomic_file(path,duplicate,strlen(duplicate),0600));CHECK(!mcp_import(a,path));
    const char *additional="{\"mcpServers\":{\"remote\":{\"url\":\"https://example.invalid/mcp\",\"disabled\":true}}}";
    CHECK(!atomic_file(path,additional,strlen(additional),0600));CHECK(mcp_import(a,path));
    loaded.reset(mcp_load(a));CHECK(json_object_object_length(loaded.get())==2);
    setenv("MCP_TEST_SECRET","must-not-leak",1);CHECK(mcp_prepare(a));unsetenv("MCP_TEST_SECRET");
    JsonPtr tools(mcp_tools(a));CHECK(alen(tools.get())==2);std::string echo,fail;
    for(size_t i=0;i<alen(tools.get());i++){
        J *function=get(at(tools.get(),i),"function");
        if(strstr(sget(function,"description")," / echo."))echo=sget(function,"name");else fail=sget(function,"name");
    }
    CHECK(!echo.empty()&&!fail.empty());CHECK(echo.size()<=64);
    JsonPtr input(json_tokener_parse("{\"value\":{\"count\":3,\"ok\":true,\"list\":[1,2]}}"));
    a.yes=false;JsonPtr denied(mcp_call(a,echo.c_str(),input.get()));CHECK(bget(denied.get(),"denied"));
    a.yes=true;JsonPtr result(mcp_call(a,echo.c_str(),input.get()));CHECK(!tool_failed(result.get()));
    CHECK(json_object_equal(get(result.get(),"structuredContent"),input.get()));CHECK(!bget(result.get(),"inheritedSecret"));
    CHECK(!strcmp(sget(result.get(),"explicitEnv"),"explicit-test-value"));pid_t child=iget(result.get(),"pid",0);CHECK(child>0);
    JsonPtr failed(mcp_call(a,fail.c_str(),input.get()));CHECK(tool_failed(failed.get()));
    CHECK(!workflow(&a,{model_tool("external",echo.c_str(),copy(input.get())),model_reply(nullptr,"MCP model integration complete")}));
    JsonPtr request(capture(&a,0));J *defs=get(get(request.get(),"body"),"tools");CHECK(alen(defs)==8);
    JsonPtr follow(capture(&a,1));J *messages=get(get(follow.get(),"body"),"messages");CHECK(strstr(sget(at(messages,alen(messages)-1),"content"),"MCP_ECHO_OK"));
    mcp_close(a);CHECK(kill(child,0)<0&&errno==ESRCH);
    put(get(loaded.get(),"fixture"),"disabled",json_object_new_boolean(true));CHECK(mcp_save(a,loaded.get()));CHECK(mcp_prepare(a));tools.reset(mcp_tools(a));CHECK(alen(tools.get())==0);
    app_cleanup(&a);
}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--mcp-fixture"))return mcp_fixture();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    char directory[]="/tmp/desk-c-test-XXXXXX";
    CHECK(mkdtemp(directory));
    strcpy(sandbox,directory);
    test_mcp();
    test_http_reuse();
    test_tool_configuration();
    test_approval_modes();
    test_reasoning_modes();
    test_assisted_worker();
    test_chat_mode_menu();
    test_chat_exit_paths();
    test_compact_output();
    test_exit_during_work();
    test_compact_approval();
    test_line_editing();
    test_queued_editing();
    test_conversation_picker();
    test_thinking_timer();
    test_background_conversations();
    test_utf8();
    test_files();
    test_search_and_edit();
    test_config();
    test_migration();
    test_process();
    test_context_and_validation();
    test_openai_loop();
    test_anthropic_loop();
    test_failed_task();
    test_edit_workflow();
    test_recovery_and_loops();
    test_native_error_flags();
    test_menu();
    test_parallel_tasks();
    test_parallel_resources();
    test_task_switch_approval();
    test_task_switch_approval(false);
    test_language_preferences();
    test_english_menu();
    test_no_extra_model_prompts();
    test_numeric_request();
    test_numeric_settings();
    curl_global_cleanup();
    printf("PASS: %d assertions; file/search/edit/config/process/context/recovery/OpenAI/Anthropic/menu/parallel/resume/approval/locks integration.\n",checks);
    return 0;
}
