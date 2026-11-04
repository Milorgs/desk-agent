#include "desk.hpp"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
static void on_signal(int number){
    interrupted=number;
}
static void status(App *a){
    printf("\n  desk %s\n  当前模型：%s / %s\n  接口：%s\n  工作目录：%s\n  任务：%s\n  桌面：%s  操作：%s\n",VERSION,sget(a->config,"profile"),sget(a->config,"model"),sget(a->config,"protocol"),a->root,a->state?a->state:"等待任务",a->desktop?"开启":"关闭",a->yes?"自动批准":"执行前确认");
}
static char *field(const char *name,const char *fallback,bool secret){
    char prompt[8192];
    if(secret)snprintf(prompt,sizeof prompt,"%s%s：",name,fallback&&*fallback?" [回车保留]":"");
    else snprintf(prompt,sizeof prompt,"%s [%s]：",name,fallback?fallback:"");
    return input(prompt,fallback,secret);
}
static const char *choose_protocol(const char *current){
    puts("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 退出");
    const char *fallback=!strcmp(current,"anthropic")?"2":"1";
    for(;;){
        char *choice=field("请选择",fallback,false);
        if(!choice)return NULL;
        const char *out=!strcmp(choice,"1")||!strcmp(choice,"openai")?"openai":!strcmp(choice,"2")||!strcmp(choice,"anthropic")?"anthropic":NULL;
        bool quit=!strcmp(choice,"0");
        free(choice);
        if(out||quit)return out;
        puts("请输入 1、2 或 0。");
    }
}
static int configure(App *a,const char *name,const char *selected,bool fresh){
    if(!isatty(STDIN_FILENO)){
        seterr(a,"配置向导需要交互终端；也可通过 DESK_* 环境变量配置。");
        return -1;
    }
    if(!config_name_valid(name)){
        seterr(a,"模型配置名无效，不含空格或路径符号。");
        return -1;
    }
    if(fresh&&get(get(a->store,"profiles"),name)){
        seterr(a,"配置名已存在，请选择另一个名称。");
        return -1;
    }
    JsonPtr candidate_owner(fresh?config_default(name):copy(a->config));
    J *candidate=candidate_owner.get();
    if(!candidate)return -1;
    const char *protocol=selected?selected:choose_protocol(sget(candidate,"protocol"));
    if(!protocol){
        return 0;
    }
    bool changed=strcmp(sget(candidate,"protocol"),protocol)!=0;
    put(candidate,"protocol",str(protocol));
    if(changed)put(candidate,"base_url",str(!strcmp(protocol,"anthropic")?"https://api.anthropic.com/v1":"https://api.openai.com/v1"));
    puts("\n密钥输入不显示，仅保存在本机私有配置文件。");
    char *base=field("API 地址",sget(candidate,"base_url"),false);
    if(!base)return 0;
    bool host_changed=changed;
    CURLU *old=curl_url(),*next=curl_url();
    char *h1=NULL,*h2=NULL,*s1=NULL,*s2=NULL,*p1=NULL,*p2=NULL;
    curl_url_set(old,CURLUPART_URL,sget(a->config,"base_url"),0);
    curl_url_set(next,CURLUPART_URL,base,0);
    curl_url_get(old,CURLUPART_HOST,&h1,0);
    curl_url_get(next,CURLUPART_HOST,&h2,0);
    curl_url_get(old,CURLUPART_SCHEME,&s1,0);
    curl_url_get(next,CURLUPART_SCHEME,&s2,0);
    curl_url_get(old,CURLUPART_PORT,&p1,CURLU_DEFAULT_PORT);
    curl_url_get(next,CURLUPART_PORT,&p2,CURLU_DEFAULT_PORT);
    host_changed=host_changed||!h1||!h2||strcmp(h1,h2)||!s1||!s2||strcmp(s1,s2)||!p1||!p2||strcmp(p1,p2);
    curl_free(h1);
    curl_free(h2);
    curl_free(s1);
    curl_free(s2);
    curl_free(p1);
    curl_free(p2);
    curl_url_cleanup(old);
    curl_url_cleanup(next);
    put(candidate,"base_url",str(base));
    free(base);
    char *model=field("模型名称",sget(candidate,"model"),false);
    if(!model)return 0;
    put(candidate,"model",str(model));
    free(model);
    if(host_changed)put(candidate,"api_key",str(""));
    char *key=field("API Key",sget(candidate,"api_key"),true);
    if(!key)return 0;
    put(candidate,"api_key",str(key));
    free(key);
    if(config_valid(a,candidate)){
        return -1;
    }
    puts("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消");
    for(;;){
        char *choice=field("请选择","1",false);
        if(!choice)return 0;
        int action=!strcmp(choice,"1")?1:!strcmp(choice,"2")?2:!strcmp(choice,"0")?0:-1;
        free(choice);
        if(!action)return 0;
        if(action<0){
            puts("请输入 1、2 或 0。");
            continue;
        }
        if(action==1&&api_check(a,candidate)){
            return -1;
        }
        if(action==2)puts("配置尚未联网验证。");
        break;
    }
    int rc=save_config(a,candidate);
    if(!rc)puts("已保存并选用该模型。");
    return rc;
}
static int add_model(App *a,const char *preset){
    const char *protocol=choose_protocol("openai");
    if(!protocol)return 0;
    char fallback[64];
    int number=1;
    do{
        snprintf(fallback,sizeof fallback,"model-%d",number++);
    }
    while(get(get(a->store,"profiles"),fallback));
    char *name=preset?strdup(preset):field("模型配置名称（输入 0 返回）",fallback,false);
    if(!name)return 0;
    int rc=strcmp(name,"0")?configure(a,name,protocol,true):0;
    free(name);
    return rc;
}
static int select_model(App *a){
    J *profiles=get(a->store,"profiles");
    if(!json_object_object_length(profiles)){
        puts("尚未保存模型，请选择 [3] 添加模型。");
        return 0;
    }
    int index=1;
    puts("\n选择模型：");
    json_object_object_foreach(profiles,name,entry){
        printf("  [%d] %s / %s (%s)\n",index++,name,sget(entry,"model"),sget(entry,"protocol"));
    }
    puts("  [0] 返回");
    for(;;){
        char *choice=input("请选择编号：","0",false);
        if(!choice)return 0;
        char *end=NULL;
        long n=strtol(choice,&end,10);
        bool valid=*choice&&end&&!*end&&n>=0&&n<index;
        free(choice);
        if(!valid){
            puts("请输入列表中的编号。");
            continue;
        }
        if(!n)return 0;
        index=1;
        json_object_object_foreach(profiles,alias,value){
            (void)value;
            if(index++==n){
                char *selected=strdup(alias);
                int rc=activate(a,selected);
                free(selected);
                return rc;
            }
        }
    }
}
static void show_models(App *a){
    J *models=api_models(a,a->config);
    if(!models){
        fprintf(stderr,"%s\n",a->error);
        return;
    }
    for(size_t i=0;i<alen(models);i++)printf("  %s\n",json_object_get_string(at(models,i)));
    puts("使用 /model 模型名称 切换；可用 /test 检查工具调用。");
    json_object_put(models);
}
static int chat(App *a){
    status(a);
    puts("\n输入需求；/menu 返回菜单，/help 查看帮助，/exit 退出。任务及工具结果会发送到配置的 API。");
    for(;;){
        interrupted=0;
        char *task=input("\n› ",NULL,false);
        if(!task){
            if(interrupted==SIGINT){
                puts("\n已取消，可以继续输入。");
                continue;
            }
            return 0;
        }
        if(!*task){
            free(task);
            continue;
        }
        if(!strcmp(task,"/exit")||!strcmp(task,"/quit")){
            free(task);
            return 0;
        }
        if(!strcmp(task,"/menu")){
            free(task);
            return 1;
        }
        if(!strcmp(task,"/help"))puts("/menu 主菜单\n/new 新对话\n/status 状态\n/config 修改配置\n/provider 列出配置\n/provider 名称 切换\n/provider add 名称 添加\n/model 列出模型\n/model 名称 切换模型\n/test 连接测试\n/desktop on|off 桌面工具\n/permissions ask|auto 操作确认\n/exit 退出");
        else if(!strcmp(task,"/new")){
            reset_session(a);
            puts("已开始新对话。");
        }
        else if(!strcmp(task,"/status"))status(a);
        else if(!strcmp(task,"/config")){
            char *name=strdup(sget(a->config,"profile"));
            if(configure(a,name,NULL,false))fprintf(stderr,"%s\n",a->error);
            free(name);
        }
        else if(!strcmp(task,"/provider")){
            J *profiles=get(a->store,"profiles");
            json_object_object_foreach(profiles,name,c){
                printf("  %s / %s (%s)\n",name,sget(c,"model"),sget(c,"protocol"));
            }
        }
        else if(!strncmp(task,"/provider add ",14)){
            if(add_model(a,task+14))fprintf(stderr,"%s\n",a->error);
        }
        else if(!strncmp(task,"/provider ",10)){
            if(activate(a,task+10))fprintf(stderr,"%s\n",a->error);
            else puts("已切换模型，历史对话已清空。");
        }
        else if(!strcmp(task,"/model")||!strcmp(task,"/models"))show_models(a);
        else if(!strncmp(task,"/model ",7)){
            J *candidate=copy(a->config);
            put(candidate,"model",str(task+7));
            if(save_config(a,candidate))fprintf(stderr,"%s\n",a->error);
            else puts("已保存新模型并清空对话。");
            json_object_put(candidate);
        }
        else if(!strcmp(task,"/test")){
            puts("发送最小工具调用测试（少量 API 用量），不发送当前会话。");
            if(api_check(a,a->config))fprintf(stderr,"%s\n",a->error);
            else puts("连接成功，工具调用已验证。测试未操作电脑。");
        }
        else if(!strcmp(task,"/desktop on")){
            a->desktop=true;
            puts("桌面工具已开启，目标应用文字会发送至 API。");
        }
        else if(!strcmp(task,"/desktop off")){
            a->desktop=false;
            desktop_clear(a);
            puts("桌面工具已关闭。");
        }
        else if(!strcmp(task,"/permissions auto")){
            a->yes=true;
            puts("已自动批准启用工具的操作。");
        }
        else if(!strcmp(task,"/permissions ask")){
            a->yes=false;
            puts("已恢复操作前确认。");
        }
        else if(task[0]=='/')puts("未知命令，请输入 /help。");
        else if(run_task(a,task))fprintf(stderr,"%s\n",a->error);
        free(task);
        if(interrupted==SIGTERM)return 0;
    }
}
static int menu(App *a){
    for(;;){
        interrupted=0;
        printf("\n  desk %s\n  当前模型：%s%s%s\n\n  [1] 开始对话\n  [2] 选择模型\n  [3] 添加模型\n  [0] 退出\n",VERSION,*sget(a->config,"model")?sget(a->config,"profile"):"未配置",*sget(a->config,"model")?" / ":"",sget(a->config,"model"));
        char *choice=field("请选择","1",false);
        if(!choice){
            if(interrupted==SIGINT){
                puts("\n已取消，返回主菜单。");
                continue;
            }
            return 0;
        }
        int rc=0;
        if(!strcmp(choice,"0")||!strcmp(choice,"/exit")){
            free(choice);
            return 0;
        }
        if(!strcmp(choice,"1")){
            if(config_valid(a,a->config))puts("请先选择 [3] 添加模型，或选择 [2] 选择模型。");
            else if(!chat(a)){
                free(choice);
                return 0;
            }
        }
        else if(!strcmp(choice,"2"))rc=select_model(a);
        else if(!strcmp(choice,"3"))rc=add_model(a,NULL);
        else puts("请输入 1、2、3 或 0。");
        free(choice);
        if(rc)fprintf(stderr,"%s\n",a->error);
        if(interrupted==SIGTERM)return 143;
    }
}
static void help(void){
    puts("desk [选项] [任务|config|doctor|inspect]\n无任务参数打开数字菜单。\n--workspace 目录\n--provider 配置名\n--yes 自动批准操作\n--no-shell 关闭命令工具\n--allow-desktop 开启桌面工具\n--app 应用名称（inspect）\n--max-steps 1..100\n--record 保存私有会话记录\n--version 查看版本\n--help 帮助");
}
static int run_cli(int argc,char **argv){
    setlocale(LC_ALL,"");
    struct sigaction action{};
    action.sa_handler=on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);
    sigaction(SIGTERM,&action,NULL);
    App a={
        .yes=false,.shell=true,.desktop=false,.record=false,.max_steps=20,.state="等待任务"
    };
    auto cleanup_app=[](App *app){app_cleanup(app);};
    std::unique_ptr<App,decltype(cleanup_app)> resources(&a,cleanup_app);
    const char *workspace=".",*profile=NULL,*task=NULL,*app="";
    int rc=0;
    for(int i=1;i<argc;i++){
        const char *arg=argv[i];
        if(!strcmp(arg,"--version")){
            puts(VERSION);
            goto cleanup;
        }
        if(!strcmp(arg,"--help")||!strcmp(arg,"-h")){
            help();
            goto cleanup;
        }
        if(!strcmp(arg,"--yes"))a.yes=true;
        else if(!strcmp(arg,"--no-shell"))a.shell=false;
        else if(!strcmp(arg,"--allow-shell"))a.shell=true;
        else if(!strcmp(arg,"--allow-desktop"))a.desktop=true;
        else if(!strcmp(arg,"--record"))a.record=true;
        else if(!strcmp(arg,"--workspace")||!strcmp(arg,"--provider")||!strcmp(arg,"--app")||!strcmp(arg,"--max-steps")||!strcmp(arg,"--shell-backend")){
            if(i+1>=argc){
                rc=2;
                fprintf(stderr,"选项缺少参数。\n");
                goto cleanup;
            }
            const char *v=argv[++i];
            if(!strcmp(arg,"--workspace"))workspace=v;
            else if(!strcmp(arg,"--provider"))profile=v;
            else if(!strcmp(arg,"--app"))app=v;
            else if(!strcmp(arg,"--shell-backend")){
                if(strcmp(v,"c")){
                    fprintf(stderr,"不支持该后端选项，请省略 --shell-backend。\n");
                    rc=2;
                    goto cleanup;
                }
            }
            else{
                char *end;
                long n=strtol(v,&end,10);
                if(*end||n<1||n>100){
                    rc=2;
                    goto cleanup;
                }
                a.max_steps=(int)n;
            }
        }
        else if(arg[0]=='-'||task){
            fprintf(stderr,"未知参数，请运行 desk --help。\n");
            rc=2;
            goto cleanup;
        }
        else task=arg;
    }
    if(!realpath(workspace,a.root)){
        fprintf(stderr,"工作目录不存在。\n");
        rc=1;
        goto cleanup;
    }
    if(store_load(&a)){
        fprintf(stderr,"%s\n",a.error);
        rc=1;
        goto cleanup;
    }
    reset_session(&a);
    if(profile){
        J *c=get(get(a.store,"profiles"),profile);
        if(!c){
            fprintf(stderr,"模型配置不存在。\n");
            rc=1;
            goto cleanup;
        }
        json_object_put(a.config);
        a.config=copy(c);
    }
    if(task&&!strcmp(task,"doctor")){
        printf("desk %s\n命令工具：%s\n桌面支持：%s\nAPI 配置：%s（未联网验证）\n",VERSION,a.shell?"开启":"关闭",desktop_compiled()?"可用，真实桌面操作仍需验证":"当前构建未启用",config_valid(&a,a.config)?"不完整":"完整");
        goto cleanup;
    }
    if(task&&!strcmp(task,"config")){
        rc=configure(&a,sget(a.config,"profile"),NULL,false)?1:0;
        if(rc)fprintf(stderr,"%s\n",a.error);
        goto cleanup;
    }
    if(task&&!strcmp(task,"inspect")){
        J *snapshot=desktop_snapshot(&a,app);
        puts(json_object_to_json_string_ext(snapshot,JSON_C_TO_STRING_PRETTY));
        rc=get(snapshot,"error")?1:0;
        json_object_put(snapshot);
        goto cleanup;
    }
    if(!task&&isatty(STDIN_FILENO)){
        rc=menu(&a);
        goto cleanup;
    }
    if(config_valid(&a,a.config)){
        fprintf(stderr,"%s 请运行 desk，在菜单中添加模型。\n",a.error);
        rc=1;
        goto cleanup;
    }
    if(task){
        status(&a);
        rc=run_task(&a,task)?1:0;
        if(rc)fprintf(stderr,"%s\n",a.error);
    }
    else chat(&a);
    cleanup:
    return interrupted==SIGTERM?143:interrupted==SIGINT?130:rc;
}
int main(int argc,char **argv){
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK){
        fputs("无法初始化网络组件。\n",stderr);
        return 1;
    }
    int result=1;
    try { result=run_cli(argc,argv); }
    catch(const std::bad_alloc &) { fputs("内存不足，本次任务已停止。\n",stderr); }
    catch(const std::exception &) { fputs("程序发生内部错误，本次任务已停止。\n",stderr); }
    curl_global_cleanup();
    return result;
}
