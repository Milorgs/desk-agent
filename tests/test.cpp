#include "desk.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <pty.h>
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
    CHECK(alen(ctx)==14);
    CHECK(alen(a.messages)==22);
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
/* A local HTTP server; no external model or real credential. */ static pid_t fake_server(App *a,const char **responses,int count){
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
        for(int i=0;i<count;i++){
            int client=accept(fd,NULL,NULL);
            if(client<0)_exit(2);
            FILE *stream=fdopen(client,"r+");
            if(!stream)_exit(2);
            char *line=NULL;
            size_t cap=0;
            long content_length=0;
            char request_path[1024]="";
            J *capture=obj();
            if(getline(&line,&cap,stream)<0)_exit(2);
            sscanf(line,"%*s %1023s",request_path);
            put(capture,"path",str(request_path));
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
            fprintf(stream,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",strlen(responses[i]),responses[i]);
            fclose(stream);
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
    put(edit,"new_text",str("新的欢迎语"));
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
    CHECK(strstr(result,"新的欢迎语")&&strstr(result,"保留这行"));
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
        "{\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"signed-value\"},{\"type\":\"tool_use\",\"id\":\"write\",\"name\":\"write_file\",\"input\":{\"path\":\"hello.txt\",\"content\":\"native C\",\"expected_sha256\":null}},{\"type\":\"tool_use\",\"id\":\"read\",\"name\":\"read_file\",\"input\":{\"path\":\"hello.txt\"}}]}", "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Native done.\"}]}"
    };
    pid_t child=fake_server(&a,responses,2);
    int result=run_task(&a,"native create and read");
    server_done(child);
    CHECK(result==0);
    J *r=capture(&a,1);
    CHECK(!get(r,"authorization"));
    CHECK(get(r,"key"));
    CHECK(get(r,"version"));
    CHECK(!strcmp(sget(r,"path"),"/v1/messages"));
    J *body=get(r,"body"),*messages=get(body,"messages");
    CHECK(get(body,"system"));
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
    const char *keys="3\n0\n0\n";
    CHECK(write(master,keys,strlen(keys))==(ssize_t)strlen(keys));
    char output[16384];
    size_t used=0;
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
int main(void){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    char directory[]="/tmp/desk-c-test-XXXXXX";
    CHECK(mkdtemp(directory));
    strcpy(sandbox,directory);
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
    curl_global_cleanup();
    printf("PASS: %d assertions; file/search/edit/config/process/context/recovery/OpenAI/Anthropic/menu integration.\n",checks);
    return 0;
}
