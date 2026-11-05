#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <wchar.h>
#endif
#include <locale.h>
#include <algorithm>
#include <cstdarg>

#ifndef _WIN32
static void on_signal(int number){
    interrupted=number;
}
#endif
// One alternate screen owns every interactive page, including the main menu.
static bool interface_active=false;
static int content_top=1;
static bool roomy_interface(){
    return interface_active&&terminal_columns()>=70&&terminal_rows()-content_top+1>=30;
}
static void menu_text(const char *text){
    const bool roomy=roomy_interface(),color=interface_active&&terminal_supports_color();
    bool heading=*text=='\n';
    std::string_view rest(text);
    do{
        auto end=rest.find('\n');auto line=rest.substr(0,end);
        bool option=line.starts_with("  [");
        bool title=heading&&!line.empty()&&line.front()!=' ';
        if(interface_active&&(option||title))fputs(title?"    ":"  ",stdout);
        if(color&&(option||title))fputs(title?"\033[1;94m":"\033[1m",stdout);
        fwrite(line.data(),1,line.size(),stdout);
        if(color&&(option||title))fputs("\033[0m",stdout);
        putchar('\n');
        if(roomy&&(option||title))putchar('\n');
        if(!line.empty())heading=false;
        if(end==std::string_view::npos)break;
        rest.remove_prefix(end+1);
    }while(true);
}
static void menu_printf(const char *format,...){
    va_list args;va_start(args,format);va_list sizing;va_copy(sizing,args);
    int count=vsnprintf(nullptr,0,format,sizing);va_end(sizing);
    if(count>=0){std::vector<char> text(static_cast<size_t>(count)+1);vsnprintf(text.data(),text.size(),format,args);if(count&&text[static_cast<size_t>(count)-1]=='\n')text[static_cast<size_t>(count)-1]=0;menu_text(text.data());}
    va_end(args);
}
static void page_rule(){
    if(!roomy_interface())return;
    bool color=terminal_supports_color();
    fputs(color?"  \033[94m":"  ",stdout);
    for(int i=0;i<std::min(terminal_columns()-4,100);i++)fputs("─",stdout);
    fputs(color?"\033[0m\n":"\n",stdout);
}
static void clear_page(){
    if(interface_active){printf("\033[%d;1H\033[J",content_top);fflush(stdout);}
}
class InterfaceScreen {
public:
    InterfaceScreen(){
        interface_active=terminal_supports_ansi();
        if(interface_active){fputs("\033[?1049h\033[r\033[2J\033[H",stdout);fflush(stdout);}
    }
    ~InterfaceScreen(){
        if(interface_active){fputs("\033[r\033[0m\033[?1049l",stdout);fflush(stdout);}
        interface_active=false;content_top=1;
    }
};
static void status(App *a){
    printf(tr("\n  desk-agent %s\n  当前模型：%s\n  接口：%s\n  工作目录：%s\n  批准模式：%s\n"),VERSION,sget(a->config,"model"),sget(a->config,"protocol"),a->root,permission_name(*a));
    printf(tr("  推理模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")));
}
static char *field(const char *name,const char *fallback,bool secret){
    char prompt[8192];
    if(secret)snprintf(prompt,sizeof prompt,tr("%s%s："),name,fallback&&*fallback?tr(" [回车保留]"):"");
    else snprintf(prompt,sizeof prompt,tr("%s [%s]："),name,fallback?fallback:"");
    return input(prompt,fallback,secret);
}
static const char *choose_protocol(const char *current){
    clear_page();
    menu_text(tr("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 返回"));
    const char *fallback=!strcmp(current,"anthropic")?"2":"1";
    for(;;){
        char *choice=field(tr("请选择"),fallback,false);
        if(!choice)return NULL;
        const char *out=!strcmp(choice,"1")||!strcmp(choice,"openai")?"openai":!strcmp(choice,"2")||!strcmp(choice,"anthropic")?"anthropic":NULL;
        bool quit=!strcmp(choice,"0");
        free(choice);
        if(out||quit)return out;
        menu_text(tr("请输入 1、2 或 0。"));
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
    menu_text(tr("\n密钥输入不显示，仅保存在本机私有配置文件。"));
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
    menu_text(tr("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消"));
    for(;;){
        char *choice=field(tr("请选择"),"1",false);
        if(!choice)return 0;
        int action=!strcmp(choice,"1")?1:!strcmp(choice,"2")?2:!strcmp(choice,"0")?0:-1;
        free(choice);
        if(!action)return 0;
        if(action<0){
            menu_text(tr("请输入 1、2 或 0。"));
            continue;
        }
        if(action==1&&api_check(a,candidate)){
            return -1;
        }
        if(action==2)menu_text(tr("配置尚未联网验证。"));
        break;
    }
    int rc=save_config(a,candidate);
    if(!rc)menu_text(tr("已保存并选用该模型。"));
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
    clear_page();
    J *profiles=get(a->store,"profiles");
    if(!json_object_object_length(profiles)){
        menu_text(tr("尚未保存模型，请选择 [3] 添加模型。"));
        return 0;
    }
    std::vector<std::string> order;
    menu_text(tr("\n选择模型："));
    json_object_object_foreach(profiles,name,entry){
        order.emplace_back(name);
        menu_printf("  [%zu] %s (%s)\n",order.size(),sget(entry,"model"),sget(entry,"protocol"));
    }
    int count=static_cast<int>(order.size());
    if(count>1)menu_printf(tr("  [%d] 调整模型编号\n"),count+1);
    menu_text(tr("  [0] 返回"));
    int selected=number(jobs,count+(count>1));
    if(selected<=0)return 0;
    if(selected<=count)return activate(a,order[static_cast<size_t>(selected-1)].c_str());

    menu_text(tr("输入要调整的模型编号（0 返回）。"));
    int from=number(jobs,count);
    if(from<=0)return 0;
    menu_text(tr("输入该模型的新编号（0 返回）。"));
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
        std::string prompt=std::string(roomy_interface()?"    ":"")+tr("请选择编号：");
        std::unique_ptr<char,decltype(&free)> value(jobs.readline(prompt.c_str()),free);
        if(!value){if(interrupted==SIGINT)continue;return -1;}
        char *end=nullptr;long selected=strtol(value.get(),&end,10);
        if(*value&&!*end&&selected>=0&&selected<=maximum)return static_cast<int>(selected);
        menu_text(tr("请输入列表中的编号。"));
    }
}
static std::string request_text(Tasks &jobs,const char *prompt){
    std::unique_ptr<char,decltype(&free)> text(jobs.readline(prompt),free);
    return text?text.get():"";
}
static void language_menu(App *a,Tasks &jobs){
    clear_page();
    menu_text(tr("\n界面语言\n  [1] 简体中文\n  [2] English\n  [0] 返回"));
    int choice=number(jobs,2);
    if(choice==1)save_language(*a,"zh");
    else if(choice==2)save_language(*a,"en");
}
static void model_id_menu(App *a,Tasks &jobs){
    clear_page();
    menu_text(tr("\n切换模型 ID\n  [1] 从在线列表选择\n  [2] 手动输入\n  [0] 返回"));
    int choice=number(jobs,2);std::string model;
    if(choice==1){
        JsonPtr models(api_models(a,a->config));
        if(!models){fprintf(stderr,"%s\n",a->error);return;}
        for(size_t i=0;i<alen(models.get());i++)menu_printf("  [%zu] %s\n",i+1,json_object_get_string(at(models.get(),i)));
        menu_text(tr("  [0] 返回"));int index=number(jobs,static_cast<int>(alen(models.get())));
        if(index<=0)return;
        model=json_object_get_string(at(models.get(),static_cast<size_t>(index-1)));
    }else if(choice==2)model=request_text(jobs,tr("模型 ID（回车返回）："));
    if(model.empty())return;
    JsonPtr candidate(copy(a->config));put(candidate.get(),"model",str(model.c_str()));
    if(save_config(a,candidate.get()))fprintf(stderr,"%s\n",a->error);
    else menu_text(tr("已保存新模型并清空对话。"));
}
static void approval_menu(App *a,Tasks &jobs){
    clear_page();
    menu_text(tr("\n批准模式\n  [1] 请求批准\n  [2] 帮我批准\n  [3] 完全访问权限\n  [0] 返回"));
    menu_text(tr("请求批准：修改、命令和桌面操作前确认。帮我批准：自动批准工作目录内文件编辑，命令和桌面操作仍确认。完全访问权限：自动批准已启用工具，文件工具仍限工作目录，命令以当前用户权限运行。"));
    int selected=number(jobs,3);if(selected<=0)return;
    a->yes=selected==3;a->auto_edit=selected==2;
    printf(tr("批准模式：%s（本次运行）\n"),permission_name(*a));
}
static void reasoning_menu(App *a,Tasks &jobs){
    clear_page();
    bool native=!strcmp(sget(a->config,"protocol"),"anthropic");
    const char *openai[]={"none","minimal","low","medium","high","xhigh"};
    const char *anthropic[]={"none","low","medium","high","xhigh","max"};
    const char **levels=native?anthropic:openai;
    menu_text(tr("\n推理模式"));
    for(int i=0;i<6;i++)menu_printf("  [%d] %s (%s)\n",i+1,reasoning_name(levels[i]),levels[i]);
    menu_text(tr("  [7] 恢复服务商默认（不传推理参数）\n  [0] 返回"));
    menu_text(tr("仅发送接口原生参数；支持档位由模型决定，不自动更换模型、提高输出上限或降档。"));
    int choice=number(jobs,7);if(choice<=0)return;
    JsonPtr candidate(copy(a->config));
    if(choice==7)json_object_object_del(candidate.get(),"reasoning_effort");
    else put(candidate.get(),"reasoning_effort",str(levels[choice-1]));
    if(save_config(a,candidate.get(),false))fprintf(stderr,"%s\n",a->error);
    else printf(tr("已保存推理模式：%s；保留当前对话。\n"),reasoning_name(sget(a->config,"reasoning_effort")));
}
static void settings_menu(App *a,Tasks &jobs){
    clear_page();
    for(;;){
        menu_text(tr("\n设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] 批准模式\n  [4] 编辑当前模型配置\n  [5] 切换模型 ID\n  [6] 连接测试（少量 API 用量）\n  [7] 推理模式\n  [0] 返回"));
        int choice=number(jobs,7);if(choice<=0)return;
        if(choice==1)language_menu(a,jobs);
        else if(choice==2){
            clear_page();
            menu_text(tr("\n桌面工具\n  [1] 开启\n  [2] 关闭\n  [0] 返回"));int selected=number(jobs,2);
            if(selected==1){a->desktop=true;menu_text(tr("桌面工具已开启，目标应用文字会发送至 API。"));}
            else if(selected==2){a->desktop=false;desktop_clear(a);menu_text(tr("桌面工具已关闭。"));}
        }else if(choice==3){
            approval_menu(a,jobs);
        }else if(choice==4){
            std::string name=sget(a->config,"profile");
            if(configure(a,name.c_str(),nullptr,false))fprintf(stderr,"%s\n",a->error);
        }else if(choice==5)model_id_menu(a,jobs);
        else if(choice==6){
            menu_text(tr("发送最小工具调用测试（少量 API 用量），不发送当前会话。"));
            if(api_check(a,a->config))fprintf(stderr,"%s\n",a->error);
            else menu_text(tr("连接成功，工具调用已验证。测试未操作电脑。"));
        }
        else if(choice==7)reasoning_menu(a,jobs);
        if(interrupted==SIGTERM)return;
    }
}
struct Conversation {
    std::string title;
    JsonPtr messages,config;
    int job=0;
    bool reset_context=false;
    std::string displayed{};
    struct Request {std::string text;JsonPtr settings;};
    std::vector<Request> queue{};

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

#ifndef _WIN32
static void advance_conversations(Tasks &jobs,std::vector<Conversation> &items){
    JsonPtr states(jobs.snapshot());
    size_t active=0;
    for(size_t i=0;i<alen(states.get());i++)if(bget(at(states.get(),i),"active"))active++;
    for(auto &item:items){
        if(item.queue.empty())continue;
        J *state=nullptr;
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
        if(state&&(!strcmp(sget(state,"state"),tr("已取消"))||!strcmp(sget(state,"state"),tr("正在停止")))){
            item.queue.clear();continue;
        }
        if(!state||bget(state,"active")||active>=4)continue;
        auto &next=item.queue.front();
        if(jobs.resume(item.job,next.text,next.settings.get())){
            item.queue.erase(item.queue.begin());active++;
        }else{
            item.queue.clear();
            terminal_message(std::string(title_of(item))+" · "+tr("无法继续此对话，请新建对话。")+"\n");
        }
    }
}
class ConversationScreen {
    App &app;
    Tasks &jobs;
    std::vector<Conversation> &items;
    std::function<void()> previous;
    bool pinned=false;
    int rows=0,columns=0;
    std::string last_header;
    double checked=0;
    static std::string clipped(std::string_view text,int width){
        std::string result;int cells=0;
        while(!text.empty()){
            auto first=static_cast<unsigned char>(text.front());
            size_t size=first<0x80?1:first<0xe0?2:first<0xf0?3:4;
            if(size>text.size())break;
            wchar_t value=0;std::mbstate_t state{};
            int count=std::mbrtowc(&value,text.data(),size,&state)==size?wcwidth(value):1;
            if(count<0){text.remove_prefix(size);continue;}
            if(cells+count>width-1){result+="…";break;}
            result.append(text.substr(0,size));cells+=count;text.remove_prefix(size);
        }
        return result;
    }
public:
    Conversation *current=nullptr;
    ConversationScreen(App &a,Tasks &tasks,std::vector<Conversation> &list):app(a),jobs(tasks),items(list),previous(std::move(tasks.on_idle)){
        struct winsize size{};
        pinned=interface_active&&isatty(1)&&!ioctl(1,TIOCGWINSZ,&size)&&size.ws_row>=10&&size.ws_col>=40;
        const char *term=env_value("TERM");
        pinned=pinned&&term&&strcmp(term,"dumb");
        if(pinned){rows=size.ws_row;columns=size.ws_col;content_top=6;printf("\033[2J\033[6;%dr\033[6;1H",rows);}
        jobs.on_idle=[this]{if(previous)previous();tick();};
        tick();
    }
    ~ConversationScreen(){
        jobs.on_idle=std::move(previous);
        if(pinned)fputs("\033[r",stdout);
        content_top=1;clear_page();
        fflush(stdout);
    }
    void tick(){
        if(monotime()-checked<0.05)return;
        checked=monotime();
        struct winsize size{};
        if(pinned&&!ioctl(1,TIOCGWINSZ,&size)&&size.ws_row>0&&size.ws_col>0&&(rows!=size.ws_row||columns!=size.ws_col)){
            rows=size.ws_row;columns=size.ws_col;
            content_top=rows>=10&&columns>=40?6:1;
            printf("\0337\033[%d;%dr\0338",content_top,rows);last_header.clear();
        }
        const bool live_header=pinned&&rows>=10&&columns>=40;
        JsonPtr states(jobs.snapshot());
        std::string header=tr("对话运行状态");
        int line=2;
        for(auto &item:items){
            J *state=nullptr;
            for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
            if(state&&bget(state,"active")){
                bool thinking=bget(state,"thinking");
                auto title=live_header&&thinking?clipped(title_of(item),std::max(8,columns-42)):std::string(title_of(item));
                header+="\n"+std::string(sget(state,"state"))+" · "+title;
                if(thinking){
                    header+=" · "+std::string(tr("正在思考…"));
                    if(live_header)header+=" "+elapsed_label(json_object_get_double(get(state,"thinking_seconds")));
                }
                line++;
            }
            if(&item==current&&item.job){
                auto output=jobs.output(item.job);
                if(!output.starts_with(item.displayed))item.displayed.clear();
                auto end=output.rfind('\n');
                size_t length=end==std::string::npos?0:end+1;
                if(state&&(!bget(state,"active")||!strcmp(sget(state,"state"),tr("等待确认"))))length=output.size();
                if(length>item.displayed.size()&&valid_utf8(std::string_view(output).substr(item.displayed.size(),length-item.displayed.size()))){
                    terminal_message(output.substr(item.displayed.size(),length-item.displayed.size()));
                    item.displayed=output.substr(0,length);
                }
            }
        }
        if(header==last_header)return;
        last_header=header;
        if(live_header){
            fputs("\0337",stdout);
            size_t from=0;
            for(int row=1;row<=5;row++){
                auto end=header.find('\n',from);
                std::string_view text=from<header.size()?std::string_view(header).substr(from,end==std::string::npos?header.size()-from:end-from):std::string_view{};
                bool color=terminal_supports_color()&&row==1;
                printf("\033[%d;1H\033[2K%s%s%s",row,color?"\033[1;94m":"",clipped(text,columns).c_str(),color?"\033[0m":"");
                from=end==std::string::npos?header.size():end+1;
            }
            fputs("\0338",stdout);fflush(stdout);
        }else if(line>2)terminal_message("\n"+header+"\n");
    }
    bool active(const Conversation &item){
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)return bget(at(states.get(),i),"active");
        return false;
    }
    bool submit(Conversation &item,const std::string &task){
        app.error[0]=0;
        JsonPtr settings(conversation_settings(app,item.reset_context));
        if(!item.job){
            app.conversation_worker=true;
            item.job=jobs.start(app,task);
            app.conversation_worker=false;
            if(item.job<0){item.job=0;return false;}
        }else if(active(item)||!item.queue.empty()){
            if(item.queue.size()>=4){menu_text(tr("当前对话的待执行需求已满。"));return false;}
            item.queue.push_back({task,std::move(settings)});
            menu_text(tr("已排队，当前操作结束后继续。"));
        }else if(!jobs.resume(item.job,task,settings.get())){menu_text(tr("无法继续此对话，请新建对话。"));return false;}
        item.reset_context=false;
        checked=0;tick();return true;
    }
};
#endif
static void mcp_menu(App *a,Tasks &jobs){
    std::string notice;
    for(;;){
        clear_page();menu_text(tr("\nMCP 服务"));
        if(!notice.empty()){puts(notice.c_str());notice.clear();}
        JsonPtr servers(mcp_load(*a));
        if(!servers){fprintf(stderr,"%s\n",a->error);return;}
        std::vector<std::string> names;
        json_object_object_foreach(servers.get(),name,entry){
            names.emplace_back(name);
            menu_printf("  [%zu] %s · %s · %s\n",names.size(),name,*sget(entry,"url")?"HTTP":"stdio",tr(bget(entry,"disabled")?"已停用":"已启用"));
        }
        int count=static_cast<int>(names.size());
        menu_printf(tr("\n  [%d] 导入 JSON 配置文件\n  [%d] 添加远程 MCP 地址\n  [0] 返回\n"),count+1,count+2);
        int choice=number(jobs,count+2);if(choice<=0)return;
        a->error[0]=0;
        if(choice==count+1){
            std::string path=request_text(jobs,tr("配置文件路径（空行取消）："));if(path.empty())continue;
            if(path.starts_with("~/")){const char *home=env_value("HOME");if(home)path=std::string(home)+path.substr(1);}
            auto file=fs_path(path);if(file.is_relative())file=fs_path(a->root)/file;
            notice=mcp_import(*a,path_text(file).c_str())?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else if(choice==count+2){
            std::string name=request_text(jobs,tr("服务名称（空行取消）："));if(name.empty())continue;
            if(get(servers.get(),name.c_str())){notice=tr("服务名称已存在。");continue;}
            std::string url=request_text(jobs,tr("MCP 地址（空行取消）："));if(url.empty())continue;
            std::unique_ptr<char,decltype(&free)> token(input(tr("Bearer Token（可留空，输入隐藏）："),nullptr,true),free);
            if(!token)continue;
            JsonPtr entry(obj());put(entry.get(),"url",str(url.c_str()));
            if(*token){J *headers=obj();put(headers,"Authorization",str((std::string("Bearer ")+token.get()).c_str()));put(entry.get(),"headers",headers);}
            put(servers.get(),name.c_str(),entry.release());
            notice=mcp_save(*a,servers.get())?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else{
            const std::string name=names[static_cast<size_t>(choice-1)];J *entry=get(servers.get(),name.c_str());
            clear_page();printf("\n  MCP · %s\n",name.c_str());
            menu_text(tr("  [1] 测试连接并列出工具\n  [2] 切换启用/停用\n  [3] 删除服务\n  [0] 返回"));
            menu_text(tr("连接测试会启动配置中的本地程序，或连接远程服务。"));
            int action=number(jobs,3);
            if(action==1){
                if(mcp_prepare(*a,true,name.c_str())){
                    JsonPtr tools(mcp_tools(*a));printf(tr("连接成功，共 %zu 个工具。\n"),alen(tools.get()));
                    for(size_t i=0;i<alen(tools.get());i++)printf("  %.*s\n",180,sget(get(at(tools.get(),i),"function"),"description"));
                }else fprintf(stderr,"%s\n",a->error);
                mcp_close(*a);menu_text(tr("  [0] 返回"));number(jobs,0);
            }else if(action==2){
                put(entry,"disabled",json_object_new_boolean(!bget(entry,"disabled")));
                notice=mcp_save(*a,servers.get())?tr("MCP 设置已保存，下一条需求生效。"):a->error;
            }else if(action==3){
                menu_text(tr("删除此 MCP 配置？\n  [1] 确认删除\n  [0] 取消"));
                if(number(jobs,1)==1){json_object_object_del(servers.get(),name.c_str());notice=mcp_save(*a,servers.get())?tr("MCP 配置已删除。"):a->error;}
            }
        }
        if(interrupted==SIGTERM||input_closed)return;
    }
}
static int chat_menu(App *a,Tasks &jobs){
    clear_page();
    menu_text(tr("\n对话操作\n  [1] 新建对话\n  [2] 选择模型\n  [3] 设置\n  [4] 查看状态\n  [5] 推理模式\n  [6] 批准模式\n  [7] 返回选择对话\n  [8] MCP 服务\n  [0] 返回输入"));
    int choice=number(jobs,8);if(choice<=0)return 0;
    if(choice==1)return 3;
    else if(choice==2){if(select_model(a,jobs))fprintf(stderr,"%s\n",a->error);}
    else if(choice==3)settings_menu(a,jobs);
    else if(choice==4){
        clear_page();
        status(a);
        menu_text(tr("  [0] 返回"));
        number(jobs,0);
    }
    else if(choice==5)reasoning_menu(a,jobs);
    else if(choice==6)approval_menu(a,jobs);
    else if(choice==7)return 2;
    else if(choice==8)mcp_menu(a,jobs);
    return interrupted==SIGTERM?-1:1;
}
static bool chat(App *a,Tasks &jobs,Conversation *conversation=nullptr
#ifndef _WIN32
    ,ConversationScreen *screen=nullptr
#endif
){
    clear_page();
    if(conversation)menu_printf(tr("\n对话：%s\n"),title_of(*conversation));
    printf(tr("  工作目录：%s\n"),a->root);
    menu_text(tr("任务及工具结果会发送到配置的 API。"));
#ifndef _WIN32
    menu_text(tr(conversation?"Ctrl+D 返回选择对话并在后台继续，Ctrl+C 停止当前对话，输入 / 打开数字操作菜单。":"Ctrl+D 退出，Ctrl+C 停止当前操作，输入 / 打开数字操作菜单。"));
#else
    menu_text(tr(conversation?"Ctrl+D 返回选择对话，Ctrl+C 停止当前操作，输入 / 打开数字操作菜单。":"Ctrl+D 退出，Ctrl+C 停止当前操作，输入 / 打开数字操作菜单。"));
#endif
#ifndef _WIN32
    if(screen)screen->current=conversation;
#endif
    std::string last_model,last_modes;
    for(;;){
        if(input_closed||interrupted==SIGTERM)return false;
        if(interrupted==SIGINT)interrupted=0;
        std::string model=sget(a->config,"model");
        std::string modes=std::string(language_code())+" "+reasoning_name(sget(a->config,"reasoning_effort"))+" "+permission_name(*a);
        if(model!=last_model||modes!=last_modes){
            printf(tr("  当前模型：%s\n"),model.c_str());
            printf(tr("推理模式：%s · 批准模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")),permission_name(*a));
            last_model=std::move(model);
            last_modes=std::move(modes);
        }
        std::unique_ptr<char,decltype(&free)> line(jobs.readline(": "),free);
        if(!line){
            if(interrupted==SIGINT&&!input_closed){
#ifndef _WIN32
                if(screen&&conversation){conversation->queue.clear();jobs.cancel(conversation->job);}
#endif
                continue;
            }
            return false;
        }
        std::string task=line.get();
        if(task.empty())continue;
        if(task=="/"){
            size_t revision=a->session_revision;
            a->error[0]=0;
#ifndef _WIN32
            if(screen)screen->current=nullptr;
#endif
            int selected=chat_menu(a,jobs);
#ifndef _WIN32
            if(screen){screen->current=conversation;if(conversation)conversation->displayed.clear();}
#endif
            if(conversation&&revision!=a->session_revision)conversation->reset_context=true;
            if(selected<0||selected==2)return false;
            if(selected==3){if(conversation)return true;reset_session(a);}
            clear_page();
            if(a->error[0])fprintf(stderr,"%s\n",a->error);
            if(conversation)menu_printf(tr("\n对话：%s\n"),title_of(*conversation));
            last_modes.clear();
            continue;
        }
        if(conversation&&conversation->title.empty())conversation->title=conversation_title(task,true);
#ifndef _WIN32
        if(screen&&conversation){
            if(task.size()==1&&task[0]>='0'&&task[0]<='3'&&jobs.respond(conversation->job,task[0]-'0'))continue;
            if(!screen->submit(*conversation,task)&&a->error[0])fprintf(stderr,"%s\n",a->error);
            continue;
        }
#endif
        if(run_task(a,task.c_str())&&!input_closed)fprintf(stderr,"%s\n",a->error);
    }
}
static void show_token_usage(J *usage,J *config){
    if(usage)printf(tr("  累计 Token（接口报告）：%llu\n"),static_cast<unsigned long long>(model_tokens(usage,config)));
    else menu_text(tr("  无法读取累计 Token 用量。"));
}
static void show_banner(){
    constexpr int banner_width=59;
    const bool color=terminal_supports_color();
    const char *reset=color?"\033[0m":"";
    if(terminal_columns()<=banner_width){
        printf("\n  %sdesk-agent%s  v%s\n\n",color?"\033[3;94m":"",reset,VERSION);
        return;
    }
    constexpr const char *logo[]={
        "         __          __                               __",
        "    ____/ /__  _____/ /__       ____ _____ ___  ____  / /_",
        "   / __  / _ \\/ ___/ //_/ ___  / __ `/ __ `/ _ \\/ __ \\/ __/",
        "  / /_/ /  __(__  ) ,<   /___// /_/ / /_/ /  __/ / / / /_",
        "  \\__,_/\\___/____/_/|_|       \\__,_/\\__, /\\___/_/ /_/\\__/",
        "                                   /____/",
    };
    const char *blue=color?"\033[1;94m":"";
    int padding=roomy_interface()?2+(std::min(terminal_columns()-4,100)-banner_width)/2:0;
    putchar('\n');
    for(const auto *row:logo)printf("%*s%s%s%s\n",padding,"",blue,row,reset);
    printf("%*s\n\n",banner_width+padding,"v" VERSION);
    page_rule();
}
static void conversation_menu(App *a,Tasks &jobs,std::vector<Conversation> &items){
#ifndef _WIN32
    ConversationScreen screen(*a,jobs,items);
#endif
    bool new_chat=false;
    while(interrupted!=SIGTERM&&!input_closed){
        const int count=static_cast<int>(items.size());
        const int create=count+1,rename=count+2,remove=count+3;
        int choice=create;
        if(!new_chat){
            clear_page();
            menu_text(tr("\n选择对话（仅本次运行保留）"));
            if(items.empty())menu_text(tr("还没有对话。"));
            for(size_t i=0;i<items.size();i++)printf("  %zu. %s\n",i+1,title_of(items[i]));
            menu_printf(tr("\n  [%d] 新建对话\n  [%d] 重命名对话\n  [%d] 删除对话\n  [0] 返回主菜单\n"),create,rename,remove);
            choice=number(jobs,remove);
            if(choice<=0)return;
        }
        new_chat=false;
        if(choice==rename||choice==remove){
            if(items.empty()){
                menu_text(tr("还没有对话。"));
                continue;
            }
            bool deleting=choice==remove;
            menu_text(tr(deleting?"输入上方要删除的对话编号（0 返回）。":"输入上方要重命名的对话编号（0 返回）。"));
            int which=number(jobs,count);
            if(which<=0)continue;
            auto item=items.begin()+(which-1);
            if(deleting){
                menu_printf(tr("删除「%s」？\n  [1] 确认删除\n  [0] 取消\n"),title_of(*item));
                if(number(jobs,1)==1){
                    if(item->job)jobs.cancel(item->job);
                    items.erase(item);
                }
            }else{
                auto title=conversation_title(request_text(jobs,tr("新标题（空行取消）：")),false);
                if(!title.empty())item->title=std::move(title);
            }
            continue;
        }
        if(choice==create){
            if(config_valid(a,a->config)){menu_text(tr("请先选择 [3] 添加模型，或选择 [2] 选择模型。"));return;}
            items.push_back({{},JsonPtr(arr()),JsonPtr(copy(a->config))});
        }
        auto selected=items.begin()+(choice-1);
        auto &item=*selected;
        JsonPtr previous(a->messages);
        a->messages=item.messages.release();
        json_object_put(a->config);a->config=copy(item.config.get());
        a->state="等待任务";
        #ifndef _WIN32
        item.displayed.clear();
        new_chat=chat(a,jobs,&item,&screen);
        screen.current=nullptr;
#else
        new_chat=chat(a,jobs,&item);
#endif
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
    InterfaceScreen interface;
    std::string notice;
    Tasks jobs;
    std::vector<Conversation> conversations;
#ifndef _WIN32
    jobs.on_idle=[&]{advance_conversations(jobs,conversations);};
#endif
    for(;;){
        if(input_closed)return 0;
        clear_page();
        show_banner();
        if(!notice.empty()){puts(notice.c_str());notice.clear();}
        menu_text(tr("  已导入的模型"));
        JsonPtr usage(load_tokens(*a));
        J *profiles=get(a->store,"profiles");
        const char *active_model=sget(a->config,"model");
        const char *active_profile=sget(a->config,"profile");
        bool active_listed=false;
        if(!json_object_object_length(profiles))menu_text(tr("    暂无已导入的模型"));
        else{
            size_t index=1;
            json_object_object_foreach(profiles,name,profile){
                const char *model=sget(profile,"model");
                bool current=*active_model&&!strcmp(name,active_profile)&&!strcmp(model,active_model);
                printf("    %zu. %s",index++,model);
                if(usage)printf(" · %llu Token",static_cast<unsigned long long>(model_tokens(usage.get(),profile)));
                if(current){
                    fputs(tr(" · 当前"),stdout);
                    active_listed=true;
                }
                putchar('\n');
            }
        }
        if(!active_listed){
            if(*active_model){
                printf(tr("  当前模型：%s\n"),active_model);
                show_token_usage(usage.get(),a->config);
            }else menu_text(tr("  当前模型：未配置"));
        }
        if(!usage&&(active_listed||!*active_model))menu_text(tr("  无法读取累计 Token 用量。"));
        page_rule();
        menu_text(tr("\n  [1] 开始对话\n  [2] 选择模型\n  [3] 添加模型\n  [4] 设置\n  [5] MCP 服务\n  [0] 退出"));
        int choice=number(jobs,5);
        if(choice<0){if(interrupted==SIGINT&&!input_closed)continue;return 0;}
        if(!choice)return 0;
        int rc=0;
        if(choice==1)conversation_menu(a,jobs,conversations);
        else if(choice==2)rc=select_model(a,jobs);
        else if(choice==3)rc=add_model(a,nullptr);
        else if(choice==4)settings_menu(a,jobs);
        else if(choice==5)mcp_menu(a,jobs);
        if(rc)notice=a->error;
        if(interrupted==SIGTERM)return 143;
    }
}
static void help(void){
    menu_text(tr("运行 desk 后，使用数字菜单完成对话和设置。"));
    menu_text(tr("desk [选项] [任务|config|doctor|inspect]\n无任务参数打开数字菜单。\n--workspace 目录\n--provider 配置名\n--yes 自动批准操作\n--no-shell 关闭命令工具\n--allow-desktop 开启桌面工具\n--app 应用名称（inspect）\n--max-steps 1..100\n--record 保存私有会话记录\n--verbose 显示详细工具过程\n--version 查看版本\n--help 帮助"));
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
