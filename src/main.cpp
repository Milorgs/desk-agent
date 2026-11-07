#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include "ui.hpp"
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
#include <optional>
#include <cstdarg>
#include <cerrno>
#include <cctype>

#ifndef _WIN32
static void on_signal(int number){
    interrupted=number;
}
#endif
// One alternate screen owns every interactive page, including the main menu.
static bool interface_active=false;
static int content_top=1;
static std::string page_notice;
static void set_notice(const std::string &text){
    if(interface_active){page_notice=text;while(!page_notice.empty()&&page_notice.back()=='\n')page_notice.pop_back();}
    else puts(text.c_str());
}
static void show_notice(){
    if(!page_notice.empty()){terminal_message(page_notice+"\n\n");page_notice.clear();}
}
static bool roomy_interface(){
    // Room for the banner plus a full menu; a default 90x26 Konsole window must
    // still get rules and spacing, so keep the bar low.
    return interface_active&&terminal_columns()>=60&&terminal_rows()>=20;
}
static bool spaced_interface(){
    // Blank lines between options cost a row each. A default 90x26 Konsole
    // window fits the banner, model list and a six-item menu, so allow them.
    return roomy_interface()&&terminal_rows()>=24&&terminal_columns()>=70;
}
static void menu_text(const char *text){
    const bool roomy=spaced_interface(),color=interface_active&&terminal_supports_color();
    bool heading=*text=='\n';
    std::string_view rest(text);
    do{
        auto end=rest.find('\n');auto line=rest.substr(0,end);
        bool option=line.starts_with("  [");
        bool title=heading&&!line.empty()&&line.front()!=' ';
        if(interface_active&&(option||title))fputs(title?"    ":"  ",stdout);
        if(color&&option&&line.find("] ")!=line.npos){
            auto split=line.find("] ")+1;
            fputs(ui_paint(Tone::Dim,line.substr(0,split)).c_str(),stdout);
            fputs(ui_paint(Tone::Accent,line.substr(split)).c_str(),stdout);
        }
        else if(color&&title)fputs(ui_paint(Tone::Plain,line,true).c_str(),stdout);
        else fwrite(line.data(),1,line.size(),stdout);
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
    const int width=std::min(terminal_columns()-4,100);
    if(width<4)return;
    std::string line;
    for(int i=0;i<width;i++)line+="─";
    fputs("  ",stdout);
    fputs(ui_paint(Tone::Rule,line).c_str(),stdout);
    fputc('\n',stdout);
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
    const int width=terminal_columns()>0?std::max(24,std::min(terminal_columns()-4,88)):0;
    printf("\n  %s\n\n",ui_paint(Tone::Bold,tr("desk code 状态")).c_str());
    auto row=[&](const char *label,const char *value){
        std::string lead=tr(label);
        while(ui_width(lead)<8)lead+=' ';
        fputs("  ",stdout);
        fputs(ui_paint(Tone::Dim,lead).c_str(),stdout);
        fputs(ui_paint(Tone::Plain,ui_clip(value,width>0?std::max(12,width-10):0)).c_str(),stdout);
        putchar('\n');
    };
    row("模型",sget(a->config,"model"));
    row("接口",sget(a->config,"protocol"));
    row("目录",a->root);
    row("批准",permission_name(*a));
    row("推理",reasoning_name(sget(a->config,"reasoning_effort")));
}
static char *field(const char *name,const char *fallback,bool secret){
    char prompt[8192];
    if(secret)snprintf(prompt,sizeof prompt,tr("%s%s："),name,fallback&&*fallback?tr(" [回车保留]"):"");
    else snprintf(prompt,sizeof prompt,tr("%s [%s]："),name,fallback?fallback:"");
    return input(prompt,fallback,secret);
}
static int menu_choices(Tasks &jobs,const char *options,const char *details=nullptr,int initial=-1);
static const char *choose_protocol(Tasks &jobs,const char *current){
    clear_page();
    int choice=menu_choices(jobs,tr("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 返回"),nullptr,!strcmp(current,"anthropic")?2:1);
    return choice==1?"openai":choice==2?"anthropic":nullptr;
}
static void use_reasoning(J *config,J *value){
    if(value&&valid_reasoning_effort(config,value)&&*json_object_get_string(value))
        put(config,"reasoning_effort",copy(value));
    else json_object_object_del(config,"reasoning_effort");
}
static J *profile_reasoning(const App &app){
    J *profile=get(get(app.store,"profiles"),sget(app.config,"profile"));
    return get(profile,"reasoning_effort");
}
static J *editable_model_config(const App &app){
    J *config=copy(app.config);
    if(!app.conversation_id.empty())use_reasoning(config,profile_reasoning(app));
    return config;
}
static int configure(App *a,Tasks &jobs,const char *name,const char *selected,bool fresh){
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
    JsonPtr candidate_owner(fresh?config_default(name):editable_model_config(*a));
    J *candidate=candidate_owner.get();
    if(!candidate)return -1;
    const char *protocol=selected?selected:choose_protocol(jobs,sget(candidate,"protocol"));
    if(!protocol){
        return 0;
    }
    bool changed=strcmp(sget(candidate,"protocol"),protocol)!=0;
    put(candidate,"protocol",str(protocol));
    if(changed&&!valid_reasoning_effort(candidate,get(candidate,"reasoning_effort")))
        use_reasoning(candidate,nullptr);
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
    int action=menu_choices(jobs,tr("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消"),nullptr,1);
    if(action<=0)return 0;
    if(action==1&&api_check(a,candidate))return -1;
    if(action==2)menu_text(tr("配置尚未联网验证。"));
    int rc=save_config(a,candidate);
    if(!rc)set_notice(tr("已保存并选用该模型。"));
    return rc;
}
static int add_model(App *a,Tasks &jobs,const char *preset){
    const char *protocol=choose_protocol(jobs,"openai");
    if(!protocol)return 0;
    char fallback[64];
    int number=1;
    do{
        snprintf(fallback,sizeof fallback,"model-%d",number++);
    }
    while(get(get(a->store,"profiles"),fallback));
    char *name=preset?strdup(preset):field(tr("模型配置名称（输入 0 返回）"),fallback,false);
    if(!name)return 0;
    int rc=strcmp(name,"0")?configure(a,jobs,name,protocol,true):0;
    free(name);
    return rc;
}
static int number(Tasks &jobs,int maximum,int initial=-1);
static int arrow_menu(Tasks &jobs,const std::vector<std::string> &items,int initial=-1);
static int select_model(App *a,Tasks &jobs){
    clear_page();
    J *profiles=get(a->store,"profiles");
    if(!json_object_object_length(profiles)){
        set_notice(tr("尚未保存模型，请选择 [3] 添加模型。"));
        return 0;
    }
    std::vector<std::string> order,labels;
    menu_text(tr("\n选择模型："));
    json_object_object_foreach(profiles,name,entry){
        order.emplace_back(name);
        labels.push_back(std::string(sget(entry,"model"))+" ("+sget(entry,"protocol")+")"+(same_model(entry,a->config)?tr(" · 当前"):""));
    }
    int count=static_cast<int>(order.size());
    auto options=labels;
    if(count>1)options.emplace_back(tr("调整模型编号"));
    options.emplace_back(tr("返回"));
    int selected=arrow_menu(jobs,options);
    if(selected<=0)return 0;
    if(selected<=count){
        int result=activate(a,order[static_cast<size_t>(selected-1)].c_str());
        if(!result){
            char message[512];snprintf(message,sizeof message,tr("已选择模型：%s（下次启动默认使用）。"),sget(a->config,"model"));
            std::string notice=message;
            JsonPtr states(jobs.snapshot());
            for(size_t i=0;i<alen(states.get());i++)if(bget(at(states.get(),i),"active")){
                notice+="\n"+std::string(tr("后续及排队需求使用新模型；正在执行的任务继续使用原模型。"));break;
            }
            set_notice(notice);
        }
        return result;
    }

    menu_text(tr("输入要调整的模型编号（0 返回）。"));
    labels.emplace_back(tr("返回"));
    int from=arrow_menu(jobs,labels);
    if(from<=0)return 0;
    menu_text(tr("输入该模型的新编号（0 返回）。"));
    int to=arrow_menu(jobs,labels);
    if(to<=0||from==to)return 0;
    auto source=order.begin()+(from-1),target=order.begin()+(to-1);
    if(source<target)std::rotate(source,source+1,target+1);
    else std::rotate(target,source,source+1);
    return reorder_models(a,order);
}
static int number(Tasks &jobs,int maximum,int initial){
    for(;;){
        if(input_closed||interrupted==SIGTERM)return -1;
        interrupted=0;
        std::string prompt=std::string(roomy_interface()?"    ":"")+tr("请选择编号：");
        std::unique_ptr<char,decltype(&free)> value(jobs.readline(prompt.c_str()),free);
        if(!value){if(interrupted==SIGINT)continue;return -1;}
        char *start=value.get();while(isspace(static_cast<unsigned char>(*start)))start++;
        if(!*start&&initial>=0)return initial;
        char *end=nullptr;errno=0;long selected=strtol(start,&end,10);
        bool digits=end!=start;while(isspace(static_cast<unsigned char>(*end)))end++;
        if(digits&&errno!=ERANGE&&!*end&&selected>=0&&selected<=maximum)return static_cast<int>(selected);
        menu_text(tr("请输入列表中的编号。"));
    }
}
static std::string request_text(Tasks &jobs,const char *prompt){
    std::unique_ptr<char,decltype(&free)> text(jobs.readline(prompt),free);
    return text?text.get():"";
}
static std::optional<int> arrow_choice(Tasks &jobs,const std::vector<std::string> &items,int initial=1){
    return terminal_choice(items,initial,content_top,[&]{
        jobs.refresh();
        if(jobs.on_idle)jobs.on_idle();
    });
}
static int arrow_menu(Tasks &jobs,const std::vector<std::string> &items,int initial){
    if(auto choice=arrow_choice(jobs,items,initial<0?1:initial))return *choice;
    for(size_t i=0;i<items.size();i++)menu_printf("  [%zu] %s\n",(i+1)%items.size(),items[i].c_str());
    return number(jobs,static_cast<int>(items.size()-1),initial);
}
// Static menus keep their existing localized labels and numbered fallback.
static int menu_choices(Tasks &jobs,const char *options,const char *details,int initial){
    std::vector<std::string> items;
    std::string heading;
    for(std::string_view lines(options);!lines.empty();){
        auto end=lines.find('\n');
        auto line=lines.substr(0,end);
        auto label=line.find("] ");
        if(line.starts_with("  [")&&label!=line.npos)items.emplace_back(line.substr(label+2));
        else {heading.append(line);heading+='\n';}
        if(end==lines.npos)break;
        lines.remove_prefix(end+1);
    }
    if(!heading.empty())menu_text(heading.c_str());
    if(details)menu_text(details);
    return arrow_menu(jobs,items,initial);
}
static void language_menu(App *a,Tasks &jobs){
    clear_page();
    int choice=menu_choices(jobs,tr("\n界面语言\n  [1] 简体中文\n  [2] English\n  [0] 返回"));
    if(choice==1&&save_language(*a,"zh"))set_notice(tr("语言已切换为简体中文。"));
    else if(choice==2&&save_language(*a,"en"))set_notice(tr("语言已切换为 English。"));
}
static void model_id_menu(App *a,Tasks &jobs){
    clear_page();
    int choice=menu_choices(jobs,tr("\n切换模型 ID\n  [1] 从在线列表选择\n  [2] 手动输入\n  [0] 返回"));std::string model;
    if(choice==1){
        JsonPtr models(api_models(a,a->config));
        if(!models){fprintf(stderr,"%s\n",a->error);return;}
        std::vector<std::string> options;
        for(size_t i=0;i<alen(models.get());i++)options.emplace_back(json_object_get_string(at(models.get(),i)));
        options.emplace_back(tr("返回"));
        int index=arrow_menu(jobs,options);
        if(index<=0)return;
        model=json_object_get_string(at(models.get(),static_cast<size_t>(index-1)));
    }else if(choice==2)model=request_text(jobs,tr("模型 ID（回车返回）："));
    if(model.empty())return;
    JsonPtr candidate(editable_model_config(*a));put(candidate.get(),"model",str(model.c_str()));
    if(save_config(a,candidate.get()))fprintf(stderr,"%s\n",a->error);
    else set_notice(tr("已保存模型设置。"));
}
static void approval_menu(App *a,Tasks &jobs){
    clear_page();
    const char *options=tr("\n批准模式\n  [1] 请求批准\n  [2] 帮我批准\n  [3] 完全访问权限\n  [0] 返回");
    const char *details=tr("请求批准：修改、命令和桌面操作前确认。帮我批准：自动批准工作目录内文件编辑，命令和桌面操作仍确认。完全访问权限：自动批准已启用工具，文件工具仍限工作目录，命令以当前用户权限运行。");
    int selected=menu_choices(jobs,options,details);if(selected<=0)return;
    a->yes=selected==3;a->auto_edit=selected==2;
    char message[256];
    const char *format=a->conversation_id.empty()?tr("批准模式：%s（本次运行）\n"):tr("本对话的批准模式：%s\n");
    snprintf(message,sizeof message,format,permission_name(*a));
    set_notice(message);
}
static void reasoning_menu(App *a,Tasks &jobs){
    clear_page();
    bool native=!strcmp(sget(a->config,"protocol"),"anthropic");
    const char *openai[]={"none","minimal","low","medium","high","xhigh"};
    const char *anthropic[]={"none","low","medium","high","xhigh","max"};
    const char **levels=native?anthropic:openai;
    menu_text(tr("\n推理模式"));
    std::vector<std::string> options;
    for(int i=0;i<6;i++)options.push_back(std::string(reasoning_name(levels[i]))+" ("+levels[i]+")");
    options.emplace_back(tr("恢复服务商默认（不传推理参数）"));
    options.emplace_back(tr("返回"));
    menu_text(tr("仅发送接口原生参数；支持档位由模型决定，不自动更换模型、提高输出上限或降档。"));
    int choice=arrow_menu(jobs,options);if(choice<=0)return;
    JsonPtr candidate(copy(a->config));
    if(choice==7)json_object_object_del(candidate.get(),"reasoning_effort");
    else put(candidate.get(),"reasoning_effort",str(levels[choice-1]));
    if(!a->conversation_id.empty()){
        use_reasoning(a->config,get(candidate.get(),"reasoning_effort"));
    }else if(save_config(a,candidate.get(),false)){
        fprintf(stderr,"%s\n",a->error);
        return;
    }
    char message[256];
    snprintf(message,sizeof message,tr("已保存推理模式：%s；保留当前对话。\n"),reasoning_name(sget(a->config,"reasoning_effort")));
    set_notice(message);
}
static void mcp_menu(App *a,Tasks &jobs);
static void settings_menu(App *a,Tasks &jobs,bool from_chat=false){
    for(;;){
        clear_page();show_notice();
        const char *options=from_chat
            ?tr("\n更多设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] MCP 服务\n  [4] 编辑当前模型配置\n  [5] 连接测试（少量 API 用量）\n  [6] 切换模型 ID\n  [7] 查看状态\n  [0] 返回")
            :tr("\n设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] 批准模式\n  [4] 编辑当前模型配置\n  [5] 连接测试（少量 API 用量）\n  [0] 返回");
        int choice=menu_choices(jobs,options);
        if(choice<=0)return;
        a->error[0]=0;
        if(choice==1)language_menu(a,jobs);
        else if(choice==2){
            clear_page();
            int selected=menu_choices(jobs,tr("\n桌面工具\n  [1] 开启\n  [2] 关闭\n  [0] 返回"));
            if(selected==1){a->desktop=true;set_notice(tr("桌面工具已开启，目标应用文字会发送至 API。"));}
            else if(selected==2){a->desktop=false;desktop_clear(a);set_notice(tr("桌面工具已关闭。"));}
        }else if(choice==3){
            if(from_chat)mcp_menu(a,jobs);
            else approval_menu(a,jobs);
        }else if(choice==4){
            std::string name=sget(a->config,"profile");
            if(configure(a,jobs,name.c_str(),nullptr,false))fprintf(stderr,"%s\n",a->error);
        }else if(choice==5){
            menu_text(tr("发送最小工具调用测试（少量 API 用量），不发送当前会话。"));
            if(api_check(a,a->config))fprintf(stderr,"%s\n",a->error);
            else set_notice(tr("连接成功，工具调用已验证。测试未操作电脑。"));
        }else if(choice==6){
            model_id_menu(a,jobs);
        }else if(choice==7){
            clear_page();
            status(a);
            arrow_menu(jobs,{tr("返回")});
        }
        if(a->error[0]){set_notice(a->error);a->error[0]=0;}
        if(interrupted==SIGTERM||input_closed)return;
    }
}
struct Conversation {
    std::string title;
    JsonPtr messages,config;
    int job=0;
    bool reset_context=false;
#ifndef _WIN32
    std::string draft{};
    int draft_cursor=0;
#endif
    std::string displayed{};
    struct Request {std::string text;JsonPtr settings;};
    std::vector<Request> queue{};
    std::string id{};
    bool yes=false,auto_edit=false;
};
static bool persist_conversation(App &app,Conversation &item){
    if(item.id.empty()){char id[33];id_string(id);item.id=id;}
    const char *approval=item.yes?"full":item.auto_edit?"edit":"ask";
    if(history_save(app,item.id,item.title,item.config.get(),false,approval))return true;
    set_notice(tr("无法保存对话记录，请检查本机配置目录。"));
    return false;
}
// User messages carry the accent marker.
static void show_user_message(std::string_view body){
    // Build one string and hand it to terminal_message: that routes it into the
    // transcript region instead of drawing over the pinned input row.
    std::string text=ui_paint(Tone::Accent,"\n● ");
    size_t start=0;
    for(int line=0;;line++){
        auto end=body.find('\n',start);
        auto part=body.substr(start,end==std::string_view::npos?std::string_view::npos:end-start);
        if(line)text+="  ";
        text+=ui_paint(Tone::Plain,part);
        text+='\n';
        if(end==std::string_view::npos)break;
        start=end+1;
    }
    terminal_message(text);
}
static void show_conversation_history(App &app,const Conversation &item){
    JsonPtr record(history_read(app,item.id));
    J *messages=get(record.get(),"transcript");
    for(size_t i=0;i<alen(messages);i++){
        J *message=at(messages,i);
        const char *role=sget(message,"role");
        if(!strcmp(role,"user"))show_user_message(sget(message,"content"));
        else if(!strcmp(role,"assistant")){
            J *content=get(message,"content");
            std::string text;
            if(json_object_is_type(content,json_type_string))text=json_object_get_string(content);
            else for(size_t j=0;j<alen(content);j++){
                J *block=at(content,j);
                if(strcmp(sget(block,"type"),"text")||!*sget(block,"text"))continue;
                if(!text.empty())text+='\n';
                text+=sget(block,"text");
            }
            if(!text.empty())terminal_message(std::string("\n")+ui_assistant_marker(ui_color())+text+"\n");
        }
    }
}
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

static void sync_conversation_models(App &app,std::vector<Conversation> &items){
    for(auto &item:items){
        const bool changed=!same_model(item.config.get(),app.config);
        JsonPtr updated(copy(app.config));
        use_reasoning(updated.get(),get(item.config.get(),"reasoning_effort"));
        item.config=std::move(updated);
        if(!item.id.empty())persist_conversation(app,item);
        if(changed){
            if(item.messages)item.messages.reset(arr());
            item.reset_context=true;
        }
        for(auto &request:item.queue){
            J *config=copy(item.config.get());
            use_reasoning(config,get(get(request.settings.get(),"config"),"reasoning_effort"));
            put(request.settings.get(),"config",config);
        }
        if(!item.queue.empty()&&item.reset_context){
            // Only the first queued request starts a fresh model context.
            for(size_t i=0;i<item.queue.size();i++)put(item.queue[i].settings.get(),"reset",json_object_new_boolean(i==0));
            item.reset_context=false;
        }
    }
}

#ifndef _WIN32
static void advance_conversations(Tasks &jobs,std::vector<Conversation> &items){
    if(std::none_of(items.begin(),items.end(),[](const auto &item){return !item.queue.empty();}))return;
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
    std::string last_header,last_composer,frame;
    bool composer=false,overlay=false;
    double checked=0;
    static std::string clipped(std::string_view text,int width){
        return ui_clip(text,width);
    }
public:
    Conversation *current=nullptr;
    bool controls_open=false;
    // Two bottom rows belong to the composer status line, so the scroll region
    // stops above them and the conversation never overwrites it.
    void layout(){
        if(!pinned)return;
        content_top=!current&&rows>=10&&columns>=40?6:1;
        // Keep the inline status line when there is no room for a composer.
        composer=current&&rows>=8&&columns>=20&&!env_value("DESK_NO_COMPOSER");
        // Four fixed rows at the bottom: the box's top rule, the input line, the
        // bottom rule, then the status. The transcript scrolls above them.
        const int bottom=composer?rows-4:rows;
        printf("\033[r\033[2J\033[%d;%dr\033[%d;1H",content_top,bottom,content_top);
        set_composer_layout(composer?rows-2:0,composer?rows-4:0,content_top);
        frame.clear();
        last_composer.clear();
    }
    void select(Conversation *item){
        terminal_activity("");
        reset_transcript();
        current=item;
        last_header.clear();
        checked=0;
        layout();
        fflush(stdout);
    }
    ConversationScreen(App &a,Tasks &tasks,std::vector<Conversation> &list):app(a),jobs(tasks),items(list),previous(std::move(tasks.on_idle)){
        struct winsize size{};
        pinned=interface_active&&isatty(1)&&!ioctl(1,TIOCGWINSZ,&size)&&size.ws_row>=10&&size.ws_col>=40;
        const char *term=env_value("TERM");
        pinned=pinned&&term&&strcmp(term,"dumb");
        if(pinned){rows=size.ws_row;columns=size.ws_col;content_top=6;printf("\033[2J\033[6;%dr\033[6;1H",rows);}
        composer=false;
        jobs.on_idle=[this]{if(previous)previous();tick();};
        tick();
    }
    ~ConversationScreen(){
        jobs.on_idle=std::move(previous);
        reset_transcript();
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
            if(!overlay)layout();
            last_header.clear();
        }
        const bool live_header=!current&&pinned&&rows>=10&&columns>=40;
        J *current_state=nullptr;
        JsonPtr states(jobs.snapshot());
        struct Activity {const Conversation *item;J *state;size_t number;int priority;};
        std::vector<Activity> activity;
        size_t queued=0,approvals=0;
        for(size_t index=0;index<items.size();index++){
            auto &item=items[index];queued+=item.queue.size();
            J *state=nullptr;
            for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
            if(state){
                bool approval=bget(state,"awaiting_approval");
                if(approval)approvals++;
                int priority=approval?0:bget(state,"active")?1:!item.queue.empty()?2:3;
                activity.push_back({&item,state,index+1,priority});
            }
            if(&item==current)current_state=state;
            if(&item==current&&item.job&&!controls_open){
                auto output=jobs.output(item.job);
                if(!output.starts_with(item.displayed))item.displayed.clear();
                auto end=output.rfind('\n');
                size_t length=end==std::string::npos?0:end+1;
                if(state&&(!bget(state,"active")||!strcmp(sget(state,"state"),tr("等待确认"))))length=output.size();
                if(length>item.displayed.size()&&valid_utf8(std::string_view(output).substr(item.displayed.size(),length-item.displayed.size()))){
                    terminal_message(output.substr(item.displayed.size(),length-item.displayed.size()));
                    item.displayed=output.substr(0,length);
                }
                if(!has_composer()&&state&&bget(state,"thinking")){
                    terminal_activity(std::string(tr("正在思考…"))+" "+elapsed_label(json_object_get_double(get(state,"thinking_seconds"))));
                }else if(!has_composer()&&state&&!bget(state,"active")){
                    terminal_activity(std::string(sget(state,"state"))+" · "+title_of(item));
                }else if(!has_composer())terminal_activity("");
            }
        }
        if(has_composer()&&!controls_open)draw_chat_activity(current_state);
        draw_composer();
        if(current)return;
        // Approval requests remain visible even when several other chats run.
        std::stable_sort(activity.begin(),activity.end(),[](const auto &left,const auto &right){
            if(left.priority!=right.priority)return left.priority<right.priority;
            if(left.priority==3)return json_object_get_double(get(left.state,"changed_at"))>json_object_get_double(get(right.state,"changed_at"));
            return false;
        });
        struct Row {std::string text;bool approval=false;};
        std::vector<Row> rows_text{{tr("对话运行状态"),approvals>0}};
        if(approvals){char label[128];snprintf(label,sizeof label,tr(" · 待审批：%zu（进入对应对话处理）"),approvals);rows_text[0].text+=label;}
        if(queued){char label[80];snprintf(label,sizeof label,tr(" · 排队：%zu"),queued);rows_text[0].text+=label;}
        for(const auto &entry:activity){
            if(rows_text.size()==5)break;
            const auto &item=*entry.item;J *state=entry.state;
            bool approval=bget(state,"awaiting_approval"),thinking=bget(state,"thinking");
            const char *status=approval?tr("等待审批"):entry.priority==2?tr("排队中"):sget(state,"state");
            std::string prefix="["+std::to_string(entry.number)+"] "+status+" · ";
            auto title=live_header&&thinking?clipped(title_of(item),std::max(4,columns-48)):std::string(title_of(item));
            std::string text=prefix+title;
            if(thinking){
                text+=" · "+std::string(tr("正在思考…"));
                if(live_header)text+=" "+elapsed_label(json_object_get_double(get(state,"thinking_seconds")));
            }
            rows_text.push_back({std::move(text),approval});
        }
        std::string header;
        for(const auto &row:rows_text){if(!header.empty())header+='\n';header+=row.text;}
        if(header==last_header)return;
        last_header=header;
        if(live_header){
            fputs("\0337",stdout);
            for(int row=1;row<=5;row++){
                size_t index=static_cast<size_t>(row-1);
                std::string_view text=index<rows_text.size()?rows_text[index].text:std::string_view{};
                bool approval=index<rows_text.size()&&rows_text[index].approval;
                bool color=terminal_supports_color()&&(row==1||approval);
                auto body=clipped(text,columns);
                auto painted=color?ui_paint(approval?Tone::Warning:Tone::Accent,body,true):body;
                printf("\033[%d;1H\033[2K%s",row,painted.c_str());
            }
            fputs("\0338",stdout);fflush(stdout);
        }else if(rows_text.size()>1)terminal_message("\n"+header+"\n");
    }
    void draw_chat_activity(J *state){
        if(!bget(state,"active")||bget(state,"awaiting_approval")){
            terminal_activity("");
            return;
        }
        const bool thinking=bget(state,"thinking");
        std::string text=std::string(ui_spinner(static_cast<size_t>(monotime()*9)))+" ";
        text+=tr(thinking?"正在思考…":"正在执行…");
        text+=" "+elapsed_label(json_object_get_double(get(state,thinking?"thinking_seconds":"turn_seconds")));
        auto tokens=json_object_get_uint64(get(state,"tokens"));
        if(tokens){
            char used[64];
            snprintf(used,sizeof used,tr(" · %s 词元"),token_label(tokens).c_str());
            text+=used;
        }
        terminal_activity(text);
    }
    // Bottom frame: a rule above the input line, then the input row, then the
    // modes line. The input row itself is redrawn by readline, so this only
    // paints the rule and status when they change.
    void draw_composer(){
        if(!pinned||!composer||overlay)return;
        char status[1024];
        const char *effort=sget(app.config,"reasoning_effort");
        std::string_view permission=permission_name(app);
        snprintf(status,sizeof status,tr("思考强度：%s · 批准：%s · %s"),
                 *effort?reasoning_name(effort):tr("默认"),permission.data(),sget(app.config,"model"));
        auto text=ui_clip(status,columns-4);
        std::string rule="  ";
        for(int i=0;i<std::min(columns-4,100);i++)rule+="─";
        if(text==last_composer&&rule==frame)return;
        last_composer=text;frame=rule;
        fputs("\0337",stdout);
        // Both rules share the cached string; draw the top one above the input
        // and the bottom one directly under it.
        printf("\033[%d;1H\033[2K",rows-3);
        fputs(ui_paint(Tone::Rule,rule).c_str(),stdout);
        printf("\033[%d;1H\033[2K",rows-1);
        fputs(ui_paint(Tone::Rule,rule).c_str(),stdout);
        printf("\033[%d;1H\033[2K",rows);
        std::string_view footer(text);
        auto highlight=app.yes?footer.find(permission):footer.npos;
        fputs("  ",stdout);
        if(highlight!=footer.npos){
            fputs(ui_paint(Tone::Dim,footer.substr(0,highlight)).c_str(),stdout);
            fputs(ui_paint(Tone::Orange,permission).c_str(),stdout);
            footer.remove_prefix(highlight+permission.size());
        }
        fputs(ui_paint(Tone::Dim,footer).c_str(),stdout);
        fputs("\0338",stdout);
        fflush(stdout);
    }
    bool has_composer() const {return pinned&&composer&&!overlay;}
    // Menus opened from a chat need the whole screen: drop the pinned frame and
    // its scroll region while they are up, then rebuild both afterwards.
    void begin_overlay(){
        if(!pinned||overlay)return;
        overlay=true;
        set_composer_layout(0,0);
        printf("\033[r\033[2J\033[1;1H");
        content_top=1;
        fflush(stdout);
    }
    void end_overlay(){
        if(!overlay)return;
        overlay=false;
        layout();
        refresh_composer();
        fflush(stdout);
    }
    // Approval prompts and menu redraws can overwrite the frame rows; drop the
    // cached copy so the next tick paints them again.
    void refresh_composer(){last_composer.clear();frame.clear();checked=0;tick();}
    // Show everything the worker has printed so far, ignoring the tick throttle.
    void flush(){jobs.refresh();checked=0;tick();}
    bool awaiting_approval(const Conversation &item){
        if(!item.job)return false;
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++){
            J *state=at(states.get(),i);
            if(iget(state,"id",0)==item.job)return bget(state,"awaiting_approval");
        }
        return false;
    }
    bool active(const Conversation &item){
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)return bget(at(states.get(),i),"active");
        return false;
    }
    bool submit(Conversation &item,const std::string &task){
        app.error[0]=0;
        // Echo the request into the transcript before starting the job; the
        // assistant reply streams in underneath it.
        show_user_message(task);
        JsonPtr settings(conversation_settings(app,item.reset_context));
        if(!item.job){
            persist_conversation(app,item);
            app.conversation_worker=true;
            app.guided_approval=terminal_supports_ansi()&&!env_value("DESK_NO_ARROW");
            item.job=jobs.start(app,task);
            app.guided_approval=false;
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
static std::string mcp_paste(Tasks &jobs){
    menu_text(tr("粘贴完整 JSON 后按回车，支持多行。输入空行取消。"));
    std::string text;
    for(;;){
        auto line=request_text(jobs,text.empty()?"JSON › ":"… ");
        if(line.empty()||input_closed||interrupted)return {};
        if(text.size()+line.size()+1>256000){
            menu_text(tr("MCP 配置不能超过 256 KB。"));
            return {};
        }
        text+=line;
        text+='\n';
        json_tokener *parser=json_tokener_new();
        json_tokener_set_flags(parser,JSON_TOKENER_STRICT);
        JsonPtr parsed(json_tokener_parse_ex(parser,text.data(),static_cast<int>(text.size())));
        auto error=json_tokener_get_error(parser);
        json_tokener_free(parser);
        if(error!=json_tokener_continue)return text;
    }
}
static void mcp_template(App *app,Tasks &jobs){
    const char *example="{\n  \"mcpServers\": {\n    \"my-server\": {\n      \"url\": \"https://example.com/mcp\"\n    }\n  }\n}\n";
    puts(example);
    menu_text(tr("替换服务名称和地址即可使用。留空返回，输入路径可保存示例文件。"));
    auto path=request_text(jobs,tr("保存路径："));
    if(path.empty())return;
    auto target=fs_path(path);
    if(target.is_relative())target=fs_path(app->root)/target;
    std::error_code error;
    bool exists=fs::exists(target,error);
    if(error){set_notice(tr("无法检查保存路径。"));return;}
    if(exists&&menu_choices(jobs,tr("文件已存在，是否替换？\n  [1] 替换\n  [0] 取消"),nullptr,0)!=1)return;
    if(atomic_file(path_text(target).c_str(),example,strlen(example),0600)){
        set_notice(tr("示例保存失败，请检查目录是否存在及是否可写。"));
    }else set_notice(tr("示例已保存，修改后可从 MCP 菜单导入。"));
}
static void mcp_menu(App *a,Tasks &jobs){
    std::string notice;
    for(;;){
        clear_page();menu_text(tr("\nMCP 服务"));show_notice();
        if(!notice.empty()){puts(notice.c_str());notice.clear();}
        JsonPtr servers(mcp_load(*a));
        if(!servers){fprintf(stderr,"%s\n",a->error);return;}
        std::vector<std::string> names,options;
        json_object_object_foreach(servers.get(),name,entry){
            names.emplace_back(name);
            options.push_back(std::string(name)+" · "+(*sget(entry,"url")?"HTTP":"stdio")+" · "+tr(bget(entry,"disabled")?"已停用":"已启用"));
        }
        int count=static_cast<int>(names.size());
        options.emplace_back(tr("导入 JSON 配置文件"));
        options.emplace_back(tr("添加远程 MCP 地址"));
        options.emplace_back(tr("粘贴 JSON 配置"));
        options.emplace_back(tr("查看或保存 JSON 示例"));
        options.emplace_back(tr("返回"));
        int choice=arrow_menu(jobs,options);if(choice<=0)return;
        a->error[0]=0;
        if(choice==count+1){
            std::string path=request_text(jobs,tr("配置文件路径（空行取消）："));if(path.empty())continue;
            if(path.size()>=2&&((path.front()=='"'&&path.back()=='"')||(path.front()=='\''&&path.back()=='\'')))path=path.substr(1,path.size()-2);
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
        }else if(choice==count+3){
            auto text=mcp_paste(jobs);
            if(!text.empty())notice=mcp_import_json(*a,text)?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else if(choice==count+4){
            mcp_template(a,jobs);
        }else{
            const std::string name=names[static_cast<size_t>(choice-1)];J *entry=get(servers.get(),name.c_str());
            clear_page();printf("\n  MCP · %s\n",name.c_str());
            int action=menu_choices(jobs,tr("  [1] 测试连接并列出工具\n  [2] 切换启用/停用\n  [3] 删除服务\n  [0] 返回"),
                                    tr("连接测试会启动配置中的本地程序，或连接远程服务。"));
            if(action==1){
                if(mcp_prepare(*a,true,name.c_str())){
                    JsonPtr tools(mcp_tools(*a));printf(tr("连接成功，共 %zu 个工具。\n"),alen(tools.get()));
                    for(size_t i=0;i<alen(tools.get());i++)printf("  %.*s\n",180,sget(get(at(tools.get(),i),"function"),"description"));
                }else fprintf(stderr,"%s\n",a->error);
                mcp_close(*a);arrow_menu(jobs,{tr("返回")});
            }else if(action==2){
                put(entry,"disabled",json_object_new_boolean(!bget(entry,"disabled")));
                notice=mcp_save(*a,servers.get())?tr("MCP 设置已保存，下一条需求生效。"):a->error;
            }else if(action==3){
                if(menu_choices(jobs,tr("删除此 MCP 配置？\n  [1] 确认删除\n  [0] 取消"),nullptr,0)==1){json_object_object_del(servers.get(),name.c_str());notice=mcp_save(*a,servers.get())?tr("MCP 配置已删除。"):a->error;}
            }
        }
        if(interrupted==SIGTERM||input_closed)return;
    }
}
static int chat_menu(App *a,Tasks &jobs){
    clear_page();
    const auto heading=ui_clip(tr("对话操作"),std::max(1,terminal_columns()-4));
    printf("  %s\n",ui_paint(Tone::Plain,heading,true).c_str());
    std::vector<std::string> items;
    for(const auto *label:{"选择模型","推理模式","批准模式","新建对话","返回选择对话","更多设置","返回输入"})
        items.emplace_back(tr(label));
    auto selected=terminal_choice(items,1,content_top+1,[&]{
        jobs.refresh();
        if(jobs.on_idle)jobs.on_idle();
    },true);
    if(!selected){
        for(size_t i=0;i<items.size();i++){
            auto label=ui_paint(Tone::Dim,"["+std::to_string((i+1)%items.size())+"]");
            printf("  %s %s\n",label.c_str(),items[i].c_str());
        }
        selected=number(jobs,static_cast<int>(items.size()-1),0);
    }
    int choice=*selected;
    if(choice<0)return -1;
    if(choice==0)return 0;
    if(choice==1){if(select_model(a,jobs))fprintf(stderr,"%s\n",a->error);}
    else if(choice==2)reasoning_menu(a,jobs);
    else if(choice==3)approval_menu(a,jobs);
    else if(choice==4)return 3;
    else if(choice==5)return 2;
    else if(choice==6)settings_menu(a,jobs,true);
    return interrupted==SIGTERM?-1:1;
}
// The chat title goes through the transcript writer so that, with the pinned
// input box, it takes the first transcript row instead of being overwritten.
static void chat_title(const Conversation &item){
    char line[1024];
    snprintf(line,sizeof line,tr("对话：%s"),title_of(item));
    std::string text="\n";
    text+=interface_active?"    "+ui_paint(Tone::Plain,line,true):std::string(line);
    text+="\n";
    terminal_message(text);
}
static bool chat(App *a,Tasks &jobs,Conversation *conversation=nullptr
#ifndef _WIN32
    ,ConversationScreen *screen=nullptr
#endif
){
#ifndef _WIN32
    if(screen)screen->select(conversation);
#endif
#ifndef _WIN32
    if(!screen||!screen->has_composer())
#endif
    clear_page();
    if(conversation)chat_title(*conversation);
    show_notice();
    // Orientation is one dim line, once per run, above any restored history.
    if(!conversation||conversation->messages==nullptr){
        static bool context_shown=false;
        if(!context_shown){
            context_shown=true;
            terminal_message(ui_paint(Tone::Dim,tr("  工作目录及任务会发送到配置的 API；输入需求开始，或 / 打开操作菜单"))+"\n");
        }
    }
    if(conversation)show_conversation_history(*a,*conversation);
    const bool guided=terminal_supports_ansi()&&!env_value("DESK_NO_ARROW");
    ChatDraft draft;
#ifndef _WIN32
    if(conversation){
        draft.text=std::move(conversation->draft);
        draft.cursor=conversation->draft_cursor;
    }
    if(guided&&screen&&conversation)draft.approval_pending=[&]{return screen->awaiting_approval(*conversation);};
#endif
    std::string last_model,last_modes;
    for(;;){
        if(input_closed||interrupted==SIGTERM)return false;
        if(interrupted==SIGINT)interrupted=0;
        // Without the pinned composer (pipes, narrow windows, Windows) the modes
        // have no home, so print them inline as before.
#ifndef _WIN32
        if(!screen||!screen->has_composer())
#endif
        {
            std::string model=sget(a->config,"model");
            std::string modes=std::string(language_code())+" "+reasoning_name(sget(a->config,"reasoning_effort"))+" "+permission_name(*a);
            if(model!=last_model||modes!=last_modes){
                char hint[768],summary[512];
                snprintf(hint,sizeof hint,tr("  当前模型：%s · F2 模型 · F3 推理\n"),model.c_str());
                fputs(ui_paint(Tone::Dim,hint).c_str(),stdout);
                snprintf(summary,sizeof summary,tr("推理模式：%s · 批准模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")),permission_name(*a));
                fputs(ui_paint(Tone::Dim,summary).c_str(),stdout);
                last_model=std::move(model);
                last_modes=std::move(modes);
            }
        }
#ifndef _WIN32
        const bool composer=screen&&screen->has_composer();
        if(composer){
            // Park the cursor on the frame's input row; draw_composer paints the
            // rule above it and the status below it.
            printf("\033[%d;1H\033[2K",terminal_rows()-2);
            fflush(stdout);
        }
        const char *prompt=tr("❯ ");
        std::unique_ptr<char,decltype(&free)> line((guided||composer)?editable_input(prompt,nullptr,false,[&]{
            jobs.refresh();
            if(jobs.on_idle)jobs.on_idle();
        },&draft):jobs.readline(prompt),free);
        if(guided&&draft.paused){
            // The approval panel must be on screen before the choices wait for
            // a key; otherwise it arrives later and lands on the input row.
            screen->flush();
            // The worker has just printed the request panel at the bottom of
            // the transcript; draw the choices on the input row right under it
            // so the panel and its answers read as one block.
            if(screen->has_composer()){
                printf("\033[%d;1H\033[2K",terminal_rows()-2);
                fflush(stdout);
            }
            const bool inline_choices=screen->has_composer();
            const int anchor=inline_choices?terminal_rows()-2:content_top;
            // Focus starts on Cancel so a stray Enter never approves anything.
            const auto choice=terminal_choice(approval_choices(!a->verbose),0,anchor,[&]{
                jobs.refresh();
                if(jobs.on_idle)jobs.on_idle();
            },true,inline_choices);
            if(interrupted==SIGINT){
                conversation->queue.clear();
                jobs.cancel(conversation->job);
            }else if(input_closed){
                conversation->draft=std::move(draft.text);
                conversation->draft_cursor=draft.cursor;
                return false;
            }else if(choice&&*choice>=0){
                jobs.respond(conversation->job,*choice);
            }
            screen->refresh_composer();
            continue;
        }
#else
        const char *prompt=tr("❯ ");
        std::unique_ptr<char,decltype(&free)> line(guided?editable_input(prompt,nullptr,false,[&]{
            jobs.refresh();
            if(jobs.on_idle)jobs.on_idle();
        },&draft):jobs.readline(prompt),free);
#endif
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
        if(task=="/"||task=="/model"||task=="/reasoning"){
            size_t revision=a->session_revision,config_revision=a->config_revision;
            a->error[0]=0;
#ifndef _WIN32
            if(screen){screen->controls_open=true;screen->begin_overlay();}
#endif
            int selected=1;
            if(task=="/model")select_model(a,jobs);
            else if(task=="/reasoning")reasoning_menu(a,jobs);
            else selected=chat_menu(a,jobs);
#ifndef _WIN32
            if(screen){screen->end_overlay();screen->controls_open=false;}
#endif
            if(conversation){
                if(revision!=a->session_revision)conversation->reset_context=true;
                if(config_revision!=a->config_revision)
                    use_reasoning(a->config,get(conversation->config.get(),"reasoning_effort"));
                conversation->config.reset(copy(a->config));
                conversation->yes=a->yes;
                conversation->auto_edit=a->auto_edit;
                persist_conversation(*a,*conversation);
            }
            if(selected<0||selected==2)return false;
            if(selected==3){if(conversation)return true;reset_session(a);}
#ifndef _WIN32
            if(screen&&screen->has_composer()){
                if(a->error[0])terminal_message(std::string(a->error)+"\n");
                show_notice();
                continue;
            }
#endif
            clear_page();
            if(a->error[0])fprintf(stderr,"%s\n",a->error);
            if(conversation)chat_title(*conversation);
            show_notice();
            // Put the transcript back the way opening the chat does; anything the
            // worker printed while the menu was up follows from the job output.
            if(conversation)show_conversation_history(*a,*conversation);
#ifndef _WIN32
            if(screen)screen->refresh_composer();
#endif
            last_modes.clear();
            continue;
        }
        if(conversation){
            if(conversation->title.empty())conversation->title=conversation_title(task,true);
            persist_conversation(*a,*conversation);
        }
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
    if(usage)printf(tr("  累计词元（接口报告）：%llu\n"),static_cast<unsigned long long>(model_tokens(usage,config)));
    else menu_text(tr("  无法读取累计词元用量。"));
}
static void show_banner(){
    printf("\n  %s %s\n",
           ui_paint(Tone::Accent,"desk code",true).c_str(),
           ui_paint(Tone::Dim,VERSION).c_str());
    fputs("  ",stdout);
    fputs(ui_paint(Tone::Dim,tr("终端编程助手")).c_str(),stdout);
    putchar('\n');
    page_rule();
}
static void conversation_menu(App *a,Tasks &jobs,std::vector<Conversation> &items,bool direct=false){
#ifndef _WIN32
    ConversationScreen screen(*a,jobs,items);
#endif
    bool new_chat=direct;
    while(interrupted!=SIGTERM&&!input_closed){
        const int count=static_cast<int>(items.size());
        const int create=count+1,rename=count+2,remove=count+3;
        int choice=create;
        if(!new_chat){
            clear_page();
            menu_text(tr("\n选择对话"));
            if(items.empty())menu_text(tr("还没有对话。"));
            std::vector<std::string> options;
            for(auto &item:items){
                options.emplace_back(title_of(item));
                if(!item.queue.empty()){
                    char queued[80];
                    snprintf(queued,sizeof queued,tr(" · 排队：%zu"),item.queue.size());
                    options.back()+=queued;
                }
            }
            for(auto label:{"新建对话","重命名对话","删除对话","返回主菜单"})options.emplace_back(tr(label));
            if(auto selected=arrow_choice(jobs,options)){
                choice=*selected;
            }else{
                for(size_t i=0;i<items.size();i++){
                    auto label=ui_paint(Tone::Dim,std::to_string(i+1)+".");
                    printf("  %s %s\n",label.c_str(),options[i].c_str());
                }
                menu_printf(tr("\n  [%d] 新建对话\n  [%d] 重命名对话\n  [%d] 删除对话\n  [0] 返回主菜单\n"),create,rename,remove);
                choice=number(jobs,remove);
            }
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
            std::vector<std::string> options;
            for(auto &item:items)options.emplace_back(title_of(item));
            options.emplace_back(tr("返回"));
            int which=arrow_menu(jobs,options);
            if(which<=0)continue;
            auto item=items.begin()+(which-1);
            if(deleting){
                menu_printf(tr("删除「%s」？"),title_of(*item));
                if(arrow_menu(jobs,{tr("确认删除"),tr("取消")},0)==1){
                    if(!history_save(*a,item->id,item->title,item->config.get(),true)){
                        set_notice(tr("无法删除本机对话记录。"));continue;
                    }
                    if(item->job)jobs.remove(item->job);
                    items.erase(item);
                }
            }else{
                auto title=conversation_title(request_text(jobs,tr("新标题（空行取消）：")),false);
                if(!title.empty()){item->title=std::move(title);persist_conversation(*a,*item);}
            }
            continue;
        }
        if(choice==create){
            if(config_valid(a,a->config)){set_notice(tr("请先选择 [3] 添加模型，或选择 [2] 选择模型。"));return;}
            items.push_back({{},JsonPtr(arr()),JsonPtr(copy(a->config))});
            items.back().yes=a->yes;
            items.back().auto_edit=a->auto_edit;
            persist_conversation(*a,items.back());
        }
        auto selected=items.begin()+(choice-1);
        auto &item=*selected;
        JsonPtr previous(a->messages);
        a->messages=item.messages.release();
        a->conversation_id=item.id;
        const bool previous_yes=a->yes,previous_auto_edit=a->auto_edit;
        a->yes=item.yes;
        a->auto_edit=item.auto_edit;
        json_object_put(a->config);a->config=copy(item.config.get());
        a->state="等待任务";
        #ifndef _WIN32
        item.displayed.clear();
        new_chat=chat(a,jobs,&item,&screen);
        screen.select(nullptr);
#else
        new_chat=chat(a,jobs,&item);
#endif
        item.messages.reset(a->messages);a->messages=previous.release();
        item.config.reset(copy(a->config));
        item.yes=a->yes;
        item.auto_edit=a->auto_edit;
        persist_conversation(*a,item);
        a->conversation_id.clear();
        a->yes=previous_yes;
        a->auto_edit=previous_auto_edit;
        use_reasoning(a->config,profile_reasoning(*a));
        std::rotate(items.begin(),selected,selected+1);
        if(interrupted==SIGTERM)return;
        // Terminal EOF closes the current chat. The selector gets a fresh input.
        input_closed=false;
        if(interrupted==SIGINT)interrupted=0;
    }
}
static int menu(App *a,bool direct=false){
    InterfaceScreen interface;
    std::string notice;
    Tasks jobs;
    std::vector<Conversation> conversations;
    JsonPtr saved(history_list(*a));
    for(size_t i=0;i<alen(saved.get());i++){
        J *entry=at(saved.get(),i);
        J *profile=get(get(a->store,"profiles"),sget(entry,"profile"));
        JsonPtr selected(copy(profile?profile:a->config));
        J *reasoning=get(entry,"reasoning_effort");
        if(json_object_is_type(reasoning,json_type_string))use_reasoning(selected.get(),reasoning);
        App view;
        view.config=selected.get();
        snprintf(view.config_dir,sizeof view.config_dir,"%s",a->config_dir);
        std::string id=sget(entry,"id");
        conversations.push_back({sget(entry,"title"),JsonPtr(history_context(view,id)),std::move(selected)});
        auto &conversation=conversations.back();
        conversation.id=std::move(id);
        std::string_view approval=sget(entry,"approval_mode");
        bool saved_approval=approval=="full"||approval=="edit"||approval=="ask";
        conversation.yes=saved_approval?approval=="full":a->yes;
        conversation.auto_edit=saved_approval?approval=="edit":a->auto_edit;
    }
    size_t synced_config=a->config_revision;
    jobs.on_idle=[&]{
        if(synced_config!=a->config_revision){
            sync_conversation_models(*a,conversations);
            synced_config=a->config_revision;
        }
#ifndef _WIN32
        advance_conversations(jobs,conversations);
#endif
    };
    if(direct)conversation_menu(a,jobs,conversations,true);
    for(;;){
        if(input_closed)return 0;
        clear_page();
        show_banner();
        show_notice();
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
                char row[640];
                int used=snprintf(row,sizeof row,"    %zu. %s",index++,model);
                if(usage&&used>0&&used<(int)sizeof row)used+=snprintf(row+used,sizeof row-(size_t)used,tr(" · %llu 词元"),static_cast<unsigned long long>(model_tokens(usage.get(),profile)));
                if(current&&used>0&&used<(int)sizeof row)snprintf(row+used,sizeof row-(size_t)used,"%s",tr(" · 当前"));
                // The active profile reads as accent; the rest stay quiet.
                puts(current?ui_paint(Tone::Accent,row).c_str():ui_paint(Tone::Dim,row).c_str());
                if(current)active_listed=true;
            }
        }
        if(!active_listed){
            if(*active_model){
                printf(tr("  当前模型：%s\n"),active_model);
                show_token_usage(usage.get(),a->config);
            }else menu_text(tr("  当前模型：未配置"));
        }
        if(!usage&&(active_listed||!*active_model))menu_text(tr("  无法读取累计词元用量。"));
        page_rule();
        int choice=arrow_menu(jobs,{tr("开始对话"),tr("选择模型"),tr("添加模型"),tr("MCP 服务"),tr("设置"),tr("退出")});
        if(choice<0){if(interrupted==SIGINT&&!input_closed)continue;return 0;}
        if(!choice)return 0;
        int rc=0;
        if(choice==1)conversation_menu(a,jobs,conversations);
        else if(choice==2)rc=select_model(a,jobs);
        else if(choice==3)rc=add_model(a,jobs,nullptr);
        else if(choice==4)mcp_menu(a,jobs);
        else if(choice==5)settings_menu(a,jobs);
        if(rc)notice=a->error;
        if(interrupted==SIGTERM)return 143;
    }
}
static void help(void){
    menu_text(tr("运行 desk 后，使用数字菜单完成对话和设置。"));
    menu_text(tr("desk [选项] [chat|任务|config|doctor|inspect]\n无任务参数打开数字菜单。\ndesk chat 直接进入对话。\ndesk -p \"需求\" 执行一次请求并退出。\n--workspace 目录\n--provider 配置名\n--yes 自动批准操作\n--no-shell 关闭命令工具\n--allow-desktop 开启桌面工具\n--app 应用名称（inspect）\n--max-steps 1..100\n--record 保存私有会话记录\n--verbose 显示详细工具过程\n--version 查看版本\n--help 帮助"));
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
        .yes=false,.shell=true,.desktop=false,.record=false,.max_steps=20,.state="等待任务",.stream_text={}
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
        if(!strcmp(arg,"-p")||!strcmp(arg,"--print")){
            if(i+1>=argc){
                rc=2;
                fputs(tr("-p 需要一个需求文本。\n"),stderr);
                goto cleanup;
            }
            task=argv[++i];
            continue;
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
    if(task&&!strcmp(task,"code"))task=nullptr;
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
        Tasks jobs;
        rc=configure(&a,jobs,sget(a.config,"profile"),NULL,false)?1:0;
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
    if(task&&!strcmp(task,"chat")){
        rc=menu(&a,true);
    }else if(task){
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
