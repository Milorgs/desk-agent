#include "desk.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <locale.h>
#include <algorithm>

#ifndef _WIN32
static void on_signal(int number){
    interrupted=number;
}
#endif
static void status(App *a){
    printf(tr("\n  desk %s\n  当前模型：%s / %s\n  接口：%s\n  工作目录：%s\n  批准模式：%s\n"),VERSION,sget(a->config,"profile"),sget(a->config,"model"),sget(a->config,"protocol"),a->root,permission_name(*a));
    printf(tr("  推理模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")));
}
static char *field(const char *name,const char *fallback,bool secret){
    char prompt[8192];
    if(secret)snprintf(prompt,sizeof prompt,tr("%s%s："),name,fallback&&*fallback?tr(" [回车保留]"):"");
    else snprintf(prompt,sizeof prompt,tr("%s [%s]："),name,fallback?fallback:"");
    return input(prompt,fallback,secret);
}
static const char *choose_protocol(const char *current){
    puts(tr("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 返回"));
    const char *fallback=!strcmp(current,"anthropic")?"2":"1";
    for(;;){
        char *choice=field(tr("请选择"),fallback,false);
        if(!choice)return NULL;
        const char *out=!strcmp(choice,"1")||!strcmp(choice,"openai")?"openai":!strcmp(choice,"2")||!strcmp(choice,"anthropic")?"anthropic":NULL;
        bool quit=!strcmp(choice,"0");
        free(choice);
        if(out||quit)return out;
        puts(tr("请输入 1、2 或 0。"));
    }
}
static int configure(App *a,const char *name,const char *selected,bool fresh){
    if(!isatty(STDIN_FILENO)){
        seterr(a,tr("配置向导需要交互终端；也可通过 DESK_* 环境变量配置。"));
        return -1;
    }
    if(!config_name_valid(name)){
        seterr(a,tr("模型配置名无效，不含空格或路径符号。"));
        return -1;
    }
    if(fresh&&get(get(a->store,"profiles"),name)){
        seterr(a,tr("配置名已存在，请选择另一个名称。"));
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
    puts(tr("\n密钥输入不显示，仅保存在本机私有配置文件。"));
    char *base=field(tr("API 地址"),sget(candidate,"base_url"),false);
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
    char *model=field(tr("模型名称"),sget(candidate,"model"),false);
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
    puts(tr("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消"));
    for(;;){
        char *choice=field(tr("请选择"),"1",false);
        if(!choice)return 0;
        int action=!strcmp(choice,"1")?1:!strcmp(choice,"2")?2:!strcmp(choice,"0")?0:-1;
        free(choice);
        if(!action)return 0;
        if(action<0){
            puts(tr("请输入 1、2 或 0。"));
            continue;
        }
        if(action==1&&api_check(a,candidate)){
            return -1;
        }
        if(action==2)puts(tr("配置尚未联网验证。"));
        break;
    }
    int rc=save_config(a,candidate);
    if(!rc)puts(tr("已保存并选用该模型。"));
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
    char *name=preset?strdup(preset):field(tr("模型配置名称（输入 0 返回）"),fallback,false);
    if(!name)return 0;
    int rc=strcmp(name,"0")?configure(a,name,protocol,true):0;
    free(name);
    return rc;
}
static int number(Tasks &jobs,int maximum);
static int select_model(App *a,Tasks &jobs){
    J *profiles=get(a->store,"profiles");
    if(!json_object_object_length(profiles)){
        puts(tr("尚未保存模型，请选择 [3] 添加模型。"));
        return 0;
    }
    std::vector<std::string> order;
    puts(tr("\n选择模型："));
    json_object_object_foreach(profiles,name,entry){
        order.emplace_back(name);
        printf("  [%zu] %s / %s (%s)\n",order.size(),name,sget(entry,"model"),sget(entry,"protocol"));
    }
    int count=static_cast<int>(order.size());
    if(count>1)printf(tr("  [%d] 调整模型编号\n"),count+1);
    puts(tr("  [0] 返回"));
    int selected=number(jobs,count+(count>1));
    if(selected<=0)return 0;
    if(selected<=count)return activate(a,order[static_cast<size_t>(selected-1)].c_str());

    puts(tr("输入要调整的模型编号（0 返回）。"));
    int from=number(jobs,count);
    if(from<=0)return 0;
    puts(tr("输入该模型的新编号（0 返回）。"));
    int to=number(jobs,count);
    if(to<=0||from==to)return 0;
    auto source=order.begin()+(from-1),target=order.begin()+(to-1);
    if(source<target)std::rotate(source,source+1,target+1);
    else std::rotate(target,source,source+1);
    return reorder_models(a,order);
}
static int number(Tasks &jobs,int maximum){
    for(;;){
        if(input_closed||interrupted==SIGTERM)return -1;
        interrupted=0;
        std::unique_ptr<char,decltype(&free)> value(jobs.readline(tr("请选择编号：")),free);
        if(!value){if(interrupted==SIGINT)continue;return -1;}
        char *end=nullptr;long selected=strtol(value.get(),&end,10);
        if(*value&&!*end&&selected>=0&&selected<=maximum)return static_cast<int>(selected);
        puts(tr("请输入列表中的编号。"));
    }
}
static std::string request_text(Tasks &jobs,const char *prompt){
    std::unique_ptr<char,decltype(&free)> text(jobs.readline(prompt),free);
    return text?text.get():"";
}
static void language_menu(App *a,Tasks &jobs){
    puts(tr("\n界面语言\n  [1] 简体中文\n  [2] English\n  [0] 返回"));
    int choice=number(jobs,2);
    if(choice==1)save_language(*a,"zh");
    else if(choice==2)save_language(*a,"en");
}
static void model_id_menu(App *a,Tasks &jobs){
    puts(tr("\n切换模型 ID\n  [1] 从在线列表选择\n  [2] 手动输入\n  [0] 返回"));
    int choice=number(jobs,2);std::string model;
    if(choice==1){
        JsonPtr models(api_models(a,a->config));
        if(!models){fprintf(stderr,"%s\n",a->error);return;}
        for(size_t i=0;i<alen(models.get());i++)printf("  [%zu] %s\n",i+1,json_object_get_string(at(models.get(),i)));
        puts(tr("  [0] 返回"));int index=number(jobs,static_cast<int>(alen(models.get())));
        if(index<=0)return;
        model=json_object_get_string(at(models.get(),static_cast<size_t>(index-1)));
    }else if(choice==2)model=request_text(jobs,tr("模型 ID（回车返回）："));
    if(model.empty())return;
    JsonPtr candidate(copy(a->config));put(candidate.get(),"model",str(model.c_str()));
    if(save_config(a,candidate.get()))fprintf(stderr,"%s\n",a->error);
    else puts(tr("已保存新模型并清空对话。"));
}
static void approval_menu(App *a,Tasks &jobs){
    puts(tr("\n批准模式\n  [1] 请求批准\n  [2] 帮我批准\n  [3] 完全访问权限\n  [0] 返回"));
    puts(tr("请求批准：修改、命令和桌面操作前确认。帮我批准：自动批准工作目录内文件编辑，命令和桌面操作仍确认。完全访问权限：自动批准已启用工具，文件工具仍限工作目录，命令以当前用户权限运行。"));
    int selected=number(jobs,3);if(selected<=0)return;
    a->yes=selected==3;a->auto_edit=selected==2;
    printf(tr("批准模式：%s（本次运行）\n"),permission_name(*a));
}
static void reasoning_menu(App *a,Tasks &jobs){
    bool native=!strcmp(sget(a->config,"protocol"),"anthropic");
    const char *openai[]={"none","minimal","low","medium","high","xhigh"};
    const char *anthropic[]={"none","low","medium","high","xhigh","max"};
    const char **levels=native?anthropic:openai;
    puts(tr("\n推理模式"));
    for(int i=0;i<6;i++)printf("  [%d] %s (%s)\n",i+1,reasoning_name(levels[i]),levels[i]);
    puts(tr("  [7] 恢复服务商默认（不传推理参数）\n  [0] 返回"));
    puts(tr("仅发送接口原生参数；支持档位由模型决定，不自动更换模型、提高输出上限或降档。"));
    int choice=number(jobs,7);if(choice<=0)return;
    JsonPtr candidate(copy(a->config));
    if(choice==7)json_object_object_del(candidate.get(),"reasoning_effort");
    else put(candidate.get(),"reasoning_effort",str(levels[choice-1]));
    if(save_config(a,candidate.get(),false))fprintf(stderr,"%s\n",a->error);
    else printf(tr("已保存推理模式：%s；保留当前对话。\n"),reasoning_name(sget(a->config,"reasoning_effort")));
}
static void settings_menu(App *a,Tasks &jobs){
    for(;;){
        puts(tr("\n设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] 批准模式\n  [4] 编辑当前模型配置\n  [5] 切换模型 ID\n  [6] 连接测试（少量 API 用量）\n  [7] 推理模式\n  [0] 返回"));
        int choice=number(jobs,7);if(choice<=0)return;
        if(choice==1)language_menu(a,jobs);
        else if(choice==2){
            puts(tr("\n桌面工具\n  [1] 开启\n  [2] 关闭\n  [0] 返回"));int selected=number(jobs,2);
            if(selected==1){a->desktop=true;puts(tr("桌面工具已开启，目标应用文字会发送至 API。"));}
            else if(selected==2){a->desktop=false;desktop_clear(a);puts(tr("桌面工具已关闭。"));}
        }else if(choice==3){
            approval_menu(a,jobs);
        }else if(choice==4){
            std::string name=sget(a->config,"profile");
            if(configure(a,name.c_str(),nullptr,false))fprintf(stderr,"%s\n",a->error);
        }else if(choice==5)model_id_menu(a,jobs);
        else if(choice==6){
            puts(tr("发送最小工具调用测试（少量 API 用量），不发送当前会话。"));
            if(api_check(a,a->config))fprintf(stderr,"%s\n",a->error);
            else puts(tr("连接成功，工具调用已验证。测试未操作电脑。"));
        }
        else if(choice==7)reasoning_menu(a,jobs);
        if(interrupted==SIGTERM)return;
    }
}
static void task_menu(App *a,Tasks &jobs){
    for(;;){
        puts(tr("\n任务管理\n  [1] 新建并行任务\n  [2] 在指定目录新建任务\n  [3] 选择已有任务\n  [0] 返回"));
        jobs.list();int choice=number(jobs,3);if(choice<=0)return;
        if(choice==1||choice==2){
            std::string workspace;
            if(choice==2){workspace=request_text(jobs,tr("工作目录（回车返回）："));if(workspace.empty())continue;}
            std::string task=request_text(jobs,tr("任务描述（回车返回）："));if(task.empty())continue;
            int id=jobs.start(*a,task,workspace);
            if(id<0)fprintf(stderr,"%s\n",a->error);
            else printf(tr("已启动任务 [%d]，可继续新建或选择任务。\n"),id);
        }else{
            JsonPtr items(jobs.snapshot());
            if(!alen(items.get())){puts(tr("尚无任务，请选择 [1] 新建并行任务。"));continue;}
            for(size_t i=0;i<alen(items.get());i++)printf("  [%zu] %s · %.120s\n",i+1,sget(at(items.get(),i),"state"),sget(at(items.get(),i),"task"));
            puts(tr("  [0] 返回"));int selected=number(jobs,static_cast<int>(alen(items.get())));
            if(selected>0)jobs.attach(iget(at(items.get(),static_cast<size_t>(selected-1)),"id",0));
        }
        if(interrupted==SIGTERM)return;
    }
}
struct Conversation {
    std::string title;
    JsonPtr messages,config;
};
static std::string conversation_title(std::string_view source,bool automatic){
    if(!valid_utf8(source))return {};
    auto first=source.find_first_not_of(" \t\r\n");
    if(first==std::string_view::npos)return {};
    source.remove_prefix(first);
    if(automatic){
        for(std::string_view prefix:{"请帮我","能帮我","帮我","请你","Please ","please ","Can you ","Could you "})
            if(source.starts_with(prefix)&&source.size()>prefix.size()){source.remove_prefix(prefix.size());break;}
    }
    std::string title;bool gap=false;size_t count=0;
    while(!source.empty()){
        auto byte=static_cast<unsigned char>(source.front());
        if(automatic&&!title.empty()&&(byte=='\n'||byte=='\r'||byte=='?'||byte=='!'||source.starts_with("。")||source.starts_with("？")||source.starts_with("！")))break;
        if(byte<=32||byte==127){gap=!title.empty();source.remove_prefix(1);continue;}
        size_t width=byte<0x80?1:byte<0xe0?2:byte<0xf0?3:4;
        if(count+(gap?1:0)>=48){title+="…";break;}
        if(gap){title+=' ';count++;gap=false;}
        title.append(source.substr(0,width));source.remove_prefix(width);count++;
    }
    return title;
}
static const char *title_of(const Conversation &item){return item.title.empty()?tr("新对话"):item.title.c_str();}
static int chat_menu(App *a,Tasks &jobs){
    puts(tr("\n对话操作\n  [1] 新建对话\n  [2] 选择模型\n  [3] 任务管理\n  [4] 设置\n  [5] 查看状态\n  [6] 推理模式\n  [7] 批准模式\n  [8] 返回选择对话\n  [0] 返回输入"));
    int choice=number(jobs,8);if(choice<=0)return 0;
    if(choice==1)return 3;
    else if(choice==2){if(select_model(a,jobs))fprintf(stderr,"%s\n",a->error);}
    else if(choice==3)task_menu(a,jobs);
    else if(choice==4)settings_menu(a,jobs);
    else if(choice==5)status(a);
    else if(choice==6)reasoning_menu(a,jobs);
    else if(choice==7)approval_menu(a,jobs);
    else if(choice==8)return 2;
    return interrupted==SIGTERM?-1:1;
}
static bool chat(App *a,Tasks &jobs,Conversation *conversation=nullptr){
    if(conversation)printf(tr("\n对话：%s\n"),title_of(*conversation));
    status(a);
    puts(tr("任务及工具结果会发送到配置的 API。"));
    puts(tr(conversation?"Ctrl+D 返回选择对话，Ctrl+C 停止当前操作，输入 / 打开数字操作菜单。":"Ctrl+D 退出，Ctrl+C 停止当前操作，输入 / 打开数字操作菜单。"));
    for(;;){
        if(input_closed||interrupted==SIGTERM)return false;
        if(interrupted==SIGINT)interrupted=0;
        printf(tr("推理模式：%s · 批准模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")),permission_name(*a));
        std::unique_ptr<char,decltype(&free)> line(jobs.readline(": "),free);
        if(!line){if(interrupted==SIGINT&&!input_closed)continue;return false;}
        std::string task=line.get();
        if(task.empty())continue;
        if(task=="/"){
            int selected=chat_menu(a,jobs);
            if(selected<0||selected==2)return false;
            if(selected==3){if(conversation)return true;reset_session(a);}
            continue;
        }
        if(conversation&&conversation->title.empty())conversation->title=conversation_title(task,true);
        if(run_task(a,task.c_str())&&!input_closed)fprintf(stderr,"%s\n",a->error);
    }
}
static void show_token_usage(J *usage,J *config){
    if(usage)printf(tr("  累计 Token（接口报告）：%llu\n"),static_cast<unsigned long long>(model_tokens(usage,config)));
    else puts(tr("  无法读取累计 Token 用量。"));
}
static void conversation_menu(App *a,Tasks &jobs,std::vector<Conversation> &items){
    constexpr int first_conversation=5;
    bool new_chat=false;
    while(interrupted!=SIGTERM&&!input_closed){
        int choice=1;
        if(!new_chat){
            puts(tr("\n选择对话（仅本次运行保留）"));

            if(items.empty())puts(tr("还没有对话。"));
            for(size_t i=0;i<items.size();i++)printf("  %zu. %s\n",i+first_conversation,title_of(items[i]));
            puts(tr("\n  [1] 新建对话\n  [2] 重命名对话\n  [3] 删除对话\n  [4] 返回主菜单\n  [0] 退出"));
            choice=number(jobs,static_cast<int>(items.size()+first_conversation-1));
            if(choice<0||choice==4)return;
            if(choice==0){input_closed=true;return;}
        }
        new_chat=false;
        if(choice==2||choice==3){
            if(items.empty()){
                puts(tr("还没有对话。"));
                continue;
            }
            bool deleting=choice==3;
            puts(tr(deleting?"输入上方要删除的对话编号（0 返回）。":"输入上方要重命名的对话编号（0 返回）。"));
            int which=number(jobs,static_cast<int>(items.size()+first_conversation-1));
            if(which<first_conversation)continue;
            auto item=items.begin()+(which-first_conversation);
            if(deleting){
                printf(tr("删除「%s」？\n  [1] 确认删除\n  [0] 取消\n"),title_of(*item));
                if(number(jobs,1)==1)items.erase(item);
            }else{
                auto title=conversation_title(request_text(jobs,tr("新标题（空行取消）：")),false);
                if(!title.empty())item->title=std::move(title);
            }
            continue;
        }
        if(choice==1){
            if(config_valid(a,a->config)){puts(tr("请先选择 [3] 添加模型，或选择 [2] 选择模型。"));return;}
            items.push_back({{},JsonPtr(arr()),JsonPtr(copy(a->config))});
        }
        auto selected=items.begin()+(choice==1?items.size()-1:static_cast<size_t>(choice-first_conversation));
        auto &item=*selected;
        JsonPtr previous(a->messages);
        a->messages=item.messages.release();
        json_object_put(a->config);a->config=copy(item.config.get());
        a->state="等待任务";
        new_chat=chat(a,jobs,&item);
        item.messages.reset(a->messages);a->messages=previous.release();
        item.config.reset(copy(a->config));
        std::rotate(items.begin(),selected,selected+1);
        if(interrupted==SIGTERM)return;
        // Terminal EOF closes the current chat. The selector gets a fresh input.
        input_closed=false;
        if(interrupted==SIGINT)interrupted=0;
    }
}
static int menu(App *a){
    Tasks jobs;
    std::vector<Conversation> conversations;
    for(;;){
        if(input_closed)return 0;
        printf("\n  desk %s\n",VERSION);
        puts(tr("  已导入的模型"));
        JsonPtr usage(load_tokens(*a));
        J *profiles=get(a->store,"profiles");
        if(!json_object_object_length(profiles))puts(tr("    暂无已导入的模型"));
        else{
            size_t index=1;
            json_object_object_foreach(profiles,name,profile){
                (void)name;
                printf("    %zu. %s",index++,sget(profile,"model"));
                if(usage)printf(" · %llu Token",static_cast<unsigned long long>(model_tokens(usage.get(),profile)));
                putchar('\n');
            }
        }
        if(*sget(a->config,"model"))printf(tr("  当前模型：%s\n"),sget(a->config,"model"));
        else puts(tr("  当前模型：未配置"));
        if(*sget(a->config,"model")||!usage)show_token_usage(usage.get(),a->config);
        puts(tr("\n  [1] 开始对话\n  [2] 选择模型\n  [3] 添加模型\n  [4] 任务管理\n  [5] 设置\n  [0] 退出"));
        int choice=number(jobs,5);
        if(choice<0){if(interrupted==SIGINT&&!input_closed)continue;return 0;}
        if(!choice)return 0;
        int rc=0;
        if(choice==1)conversation_menu(a,jobs,conversations);
        else if(choice==2)rc=select_model(a,jobs);
        else if(choice==3)rc=add_model(a,nullptr);
        else if(choice==4)task_menu(a,jobs);
        else if(choice==5)settings_menu(a,jobs);
        if(rc)fprintf(stderr,"%s\n",a->error);
        if(interrupted==SIGTERM)return 143;
    }
}
static void help(void){
    puts(tr("运行 desk 后，使用数字菜单完成对话、任务管理和设置。"));
    puts(tr("desk [选项] [任务|config|doctor|inspect]\n无任务参数打开数字菜单。\n--workspace 目录\n--provider 配置名\n--yes 自动批准操作\n--no-shell 关闭命令工具\n--allow-desktop 开启桌面工具\n--app 应用名称（inspect）\n--max-steps 1..100\n--record 保存私有会话记录\n--verbose 显示详细工具过程\n--version 查看版本\n--help 帮助"));
}
static int run_cli(int argc,char **argv){
    setlocale(LC_ALL,"");
    platform_initialize();
#ifndef _WIN32
    struct sigaction action{};
    action.sa_handler=on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);
    sigaction(SIGTERM,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
#endif
    if(argc==3&&!strcmp(argv[1],"--task-worker")){
        char *end=nullptr;long long fd=strtoll(argv[2],&end,10);
        return *end||fd<3?2:task_worker(static_cast<std::intptr_t>(fd));
    }
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
        if(!strcmp(arg,"--lang")){i++;continue;}
        if(!strcmp(arg,"--yes"))a.yes=true;
        else if(!strcmp(arg,"--no-shell"))a.shell=false;
        else if(!strcmp(arg,"--allow-shell"))a.shell=true;
        else if(!strcmp(arg,"--allow-desktop"))a.desktop=true;
        else if(!strcmp(arg,"--record"))a.record=true;
        else if(!strcmp(arg,"--verbose"))a.verbose=true;
        else if(!strcmp(arg,"--workspace")||!strcmp(arg,"--provider")||!strcmp(arg,"--app")||!strcmp(arg,"--max-steps")){
            if(i+1>=argc){
                rc=2;
                fputs(tr("选项缺少参数。\n"),stderr);
                goto cleanup;
            }
            const char *v=argv[++i];
            if(!strcmp(arg,"--workspace"))workspace=v;
            else if(!strcmp(arg,"--provider"))profile=v;
            else if(!strcmp(arg,"--app"))app=v;
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
            fputs(tr("未知参数，请运行 desk --help。\n"),stderr);
            rc=2;
            goto cleanup;
        }
        else task=arg;
    }
    if(!canonical_directory(workspace,a.root)){
        fputs(tr("工作目录不存在。\n"),stderr);
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
            fputs(tr("模型配置不存在。\n"),stderr);
            rc=1;
            goto cleanup;
        }
        json_object_put(a.config);
        a.config=copy(c);
    }
    if(task&&!strcmp(task,"doctor")){
        printf(tr("desk %s\n命令工具：%s\n桌面支持：%s\nAPI 配置：%s（未联网验证）\n"),VERSION,a.shell?tr("开启"):tr("关闭"),desktop_compiled()?tr("可用，真实桌面操作仍需验证"):tr("当前构建未启用"),config_valid(&a,a.config)?tr("不完整"):tr("完整"));
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
        fprintf(stderr,tr("%s 请运行 desk，在菜单中添加模型。\n"),a.error);
        rc=1;
        goto cleanup;
    }
    if(task){
        status(&a);
        rc=run_task(&a,task)?1:0;
        if(rc)fprintf(stderr,"%s\n",a.error);
    }
    else {Tasks jobs;chat(&a,jobs);}
    cleanup:
    return input_closed?0:interrupted==SIGTERM?143:interrupted==SIGINT?130:rc;
}
int desk_main(int argc,char **argv){
    platform_initialize();
    if(!language_init(argc,argv)){fputs(tr("语言必须为 zh 或 en。\n"),stderr);return 2;}
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK){
        fputs(tr("无法初始化网络组件。\n"),stderr);
        return 1;
    }
    int result=1;
    try { result=run_cli(argc,argv); }
    catch(const std::bad_alloc &) { fputs(tr("内存不足，本次任务已停止。\n"),stderr); }
    catch(const std::exception &) { fputs(tr("程序发生内部错误，本次任务已停止。\n"),stderr); }
    curl_global_cleanup();
    return result;
}

#ifdef _WIN32
int wmain(int argc,wchar_t **argv){
    std::vector<std::string> arguments;arguments.reserve(static_cast<size_t>(argc));
    for(int i=0;i<argc;i++)arguments.push_back(utf8(argv[i]));
    std::vector<char *> pointers;for(auto &arg:arguments)pointers.push_back(arg.data());pointers.push_back(nullptr);
    return desk_main(argc,pointers.data());
}
#else
int main(int argc,char **argv){return desk_main(argc,argv);}
#endif
