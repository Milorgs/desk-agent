#include "desk.hpp"
#include "tasks.hpp"
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
static bool terminal_until(int fd,std::string &output,const char *needle){
    double until=monotime()+8;
    while(monotime()<until){
        if(output.find(needle)!=std::string::npos)return true;
        pollfd p{fd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
        char buffer[4096];ssize_t n=read(fd,buffer,sizeof buffer);if(n<=0)break;
        output.append(buffer,static_cast<size_t>(n));
    }
    fprintf(stderr,"Terminal transcript:\n%s\n",output.c_str());return false;
}
static void test_task_switch_approval(void){
    App a=fresh();json_object_put(a.config);a.config=config("switch","openai");
    J *write=args("confirmed.txt");put(write,"content",str("user approved"));put(write,"expected_sha256",nullptr);
    std::string action=model_tool("write","write_file",write),done=model_reply(nullptr,"已核对");
    const char *responses[]={action.c_str(),done.c_str()};pid_t server=fake_server(&a,responses,2);
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("DESK_BASE_URL",sget(a.config,"base_url"),1);setenv("DESK_MODEL","model-test",1);setenv("DESK_API_KEY","fake-test-key",1);setenv("DESK_API_TYPE","openai",1);
        execl("./build/desk","desk","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    const char *keys="1\n/task 创建文件\n/switch 1\n";CHECK(::write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    CHECK(terminal_until(master,output,"允许？[y/N]"));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/confirmed.txt"));CHECK(access(path,F_OK)!=0);
    CHECK(::write(master,"y\n",2)==2);CHECK(terminal_until(master,output,"任务 [1]：已完成"));
    CHECK(::write(master,"/exit\n",6)==6);
    int status=0;double until=monotime()+5;pid_t ended=0;
    while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(10000);
    if(!ended){kill(child,SIGKILL);waitpid(child,&status,0);}
    CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);close(master);
    char *text=read_bytes(path,100,nullptr);CHECK(text&&!strcmp(text,"user approved"));free(text);
    CHECK(output.find("fake-test-key")==std::string::npos);
    server_done(server);app_cleanup(&a);
}
static void test_language_preferences(void){
    App a=fresh();json_object_put(a.config);a.config=config("unchanged-model","openai");
    std::string before=json_object_to_json_string_ext(a.config,JSON_C_TO_STRING_PLAIN);
    std::string store=json_object_to_json_string_ext(a.store,JSON_C_TO_STRING_PLAIN);
    J *user=obj();put(user,"role",str("user"));put(user,"content",str("Keep my request exactly."));add(a.messages,user);
    CHECK(language_command(a,"/lang en"));CHECK(!strcmp(language_code(),"en"));
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
    unsetenv("DESK_LANG");CHECK(language_command(a,"/lang zh"));CHECK(!strcmp(language_code(),"zh"));
    app_cleanup(&a);
}
static void test_english_menu(void){
    App a=fresh();int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("[2] Select model")!=std::string::npos);CHECK(output.find("[3] Add model")!=std::string::npos);
    CHECK(write(master,"3\n",2)==2);CHECK(terminal_until(master,output,"[1] OpenAI API"));
    CHECK(output.find("[2] Anthropic API")!=std::string::npos);
    const char *quit="0\n/lang zh\n0\n";CHECK(write(master,quit,strlen(quit))==static_cast<ssize_t>(strlen(quit)));
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
int main(void){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    char directory[]="/tmp/desk-c-test-XXXXXX";
    CHECK(mkdtemp(directory));
    strcpy(sandbox,directory);
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
    test_language_preferences();
    test_english_menu();
    test_no_extra_model_prompts();
    curl_global_cleanup();
    printf("PASS: %d assertions; file/search/edit/config/process/context/recovery/OpenAI/Anthropic/menu/parallel/resume/approval/locks integration.\n",checks);
    return 0;
}
