#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include "ui.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#endif
#include <ctype.h>
#include <algorithm>
void reset_session(App *a){
    a->session_revision++;
    json_object_put(a->messages);
    a->messages=arr();
    a->state="等待任务";
}
J *context(App *a,J *tools){
    size_t n=alen(a->messages),first=n;
    for(size_t i=0;i<n;i++)if(!strcmp(sget(at(a->messages,i),"role"),"user"))first=i;
    if(first==n)return copy(a->messages);
    J *selected=nullptr;
    int turns=0;
    for(size_t start=first;;){
        J *candidate=arr();
        for(size_t i=start;i<n;i++)add(candidate,copy(at(a->messages,i)));
        size_t bytes=strlen(json_object_to_json_string_ext(candidate,JSON_C_TO_STRING_PLAIN))+strlen(json_object_to_json_string_ext(tools,JSON_C_TO_STRING_PLAIN));
        if(bytes>CONTEXT_LIMIT){
            json_object_put(candidate);
            if(!selected)seterr(a,tr("本轮超过上下文预算，请拆分任务或选择新建对话。"));
            break;
        }
        json_object_put(selected);selected=candidate;
        if(start==0||turns++>=6)break;
        size_t previous=start;
        for(size_t i=start;i>0;i--)if(!strcmp(sget(at(a->messages,i-1),"role"),"user")){previous=i-1;break;}
        if(previous==start)break;
        start=previous;
    }
    return selected;
}
J *parse_tool_arguments(const char *text){
    size_t length=strlen(text);
    if(length>CONTEXT_LIMIT||!valid_utf8(std::string_view(text,length)))return nullptr;
    // json-c 0.18's STRICT string check treats non-ASCII signed chars as
    // controls. Feed it equivalent Unicode escapes while keeping strict JSON.
    std::string portable;
    portable.reserve(length);
    auto unit=[&](uint32_t code){
        constexpr char hex[]="0123456789abcdef";
        portable+="\\u";
        for(int shift=12;shift>=0;shift-=4)portable+=hex[(code>>shift)&0xf];
    };
    for(size_t i=0;i<length;){
        auto c=static_cast<unsigned char>(text[i]);
        if(c<0x80){portable+=text[i++];continue;}
        size_t width=c<0xe0?2:c<0xf0?3:4;
        uint32_t code=c&((1u<<(7-width))-1);
        for(size_t k=1;k<width;k++)code=(code<<6)|(static_cast<unsigned char>(text[i+k])&0x3f);
        if(code<=0xffff)unit(code);
        else {code-=0x10000;unit(0xd800+(code>>10));unit(0xdc00+(code&0x3ff));}
        i+=width;
    }
    struct json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    J *args=json_tokener_parse_ex(tok,portable.c_str(),static_cast<int>(portable.size())+1);
    bool valid=json_tokener_get_error(tok)==json_tokener_success&&json_object_is_type(args,json_type_object);
    size_t end=json_tokener_get_parse_end(tok);
    for(size_t i=end;i<portable.size();i++)if(!isspace(static_cast<unsigned char>(portable[i])))valid=false;
    json_tokener_free(tok);
    if(!valid){json_object_put(args);return NULL;}
    return args;
}
static int validate_calls(App *a,J *calls){
    if(calls&&!json_object_is_type(calls,json_type_array)){
        seterr(a,tr("工具调用不是数组。"));
        return -1;
    }
    if(alen(calls)>12){
        seterr(a,tr("单轮工具数量过多。"));
        return -1;
    }
    for(size_t i=0;i<alen(calls);i++){
        J *call=at(calls,i),*f=get(call,"function");
        J *raw=get(f,"arguments");
        if(!*sget(call,"id")||!*sget(f,"name")||!json_object_is_type(raw,json_type_string)||static_cast<size_t>(json_object_get_string_len(raw))!=strlen(sget(f,"arguments"))){
            seterr(a,tr("工具调用格式无效。"));
            return -1;
        }
        J *args=parse_tool_arguments(sget(f,"arguments"));
        if(!args){
            seterr(a,tr("工具参数不是完整的 JSON 对象，本轮操作未执行。"));
            return -1;
        }
        json_object_put(args);
        for(size_t k=0;k<i;k++)if(!strcmp(sget(at(calls,k),"id"),sget(call,"id"))){
            seterr(a,tr("工具编号重复，未执行本轮操作。"));
            return -1;
        }
    }
    return 0;
}
static void tool_result(App *a,const char *id,J *r){
    J *m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str(id));
    put(m,"content",str(json_object_to_json_string_ext(r,JSON_C_TO_STRING_PLAIN)));
    add(a->messages,m);
}
static void pending(App *a,J *calls,size_t start){
    for(size_t k=start;k<alen(calls);k++){
        J *r=err(tr("任务已中止，此操作未确认执行；继续前重新检查状态。"));
        tool_result(a,sget(at(calls,k),"id"),r);
        json_object_put(r);
    }
}
static bool mutation(const char *name){
    return !strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"desktop_action")||!strcmp(name,"desktop_set_text");
}
static const char *domain(const char *name){
    if(!strcmp(name,"read_file")||!strcmp(name,"write_file")||!strcmp(name,"edit_file"))return "file";
    if(!strncmp(name,"desktop_",8))return "desktop";
    return name;
}
/* A successful unrelated read must not erase a failed command or write. */
struct FailedOperation {
    std::string kind,target;
    bool mutation;
};
struct Observation {
    std::string name;
    JsonPtr args,result;
    int count=1;
};
static void recovery(App *a,std::vector<FailedOperation> &unresolved,const char *name,J *args,bool failed){
    const char *kind=domain(name),*target=sget(args,"path");
    char full[PATH_MAX];
    if(*target&&!resolve_file(a,target,full))target=full;
    auto matches=[&](const FailedOperation &entry){return entry.kind==kind&&entry.target==target;};
    if(!failed){
        std::erase_if(unresolved,[&](const FailedOperation &entry){return matches(entry)&&(!entry.mutation||mutation(name));});
        return;
    }
    for(auto &entry:unresolved){
        if(matches(entry)){
            entry.mutation=entry.mutation||mutation(name);
            return;
        }
    }
    unresolved.push_back({kind,target,mutation(name)});
}
static bool repeated(std::vector<Observation> &seen,const char *name,J *args,J *result){
    if(mutation(name)&&!tool_failed(result)){
        /* Changes invalidate old observations, so a post-edit read is fresh. */
        seen.clear();
        return false;
    }
    if(!strcmp(name,"run_shell")&&!tool_failed(result))return false;
    for(auto &entry:seen){
        if(entry.name!=name||!json_object_equal(entry.args.get(),args))continue;
        entry.count=json_object_equal(entry.result.get(),result)?entry.count+1:1;
        entry.result.reset(copy(result));
        return entry.count>=3;
    }
    seen.push_back({name,JsonPtr(copy(args)),JsonPtr(copy(result))});
    return false;
}
static void show_call(const char *name,J *args){
    const char *label=!strcmp(name,"read_file")?tr("读取文件"):!strcmp(name,"list_files")?tr("查看目录"):
        !strcmp(name,"search_text")?tr("搜索文本"):!strcmp(name,"edit_file")?tr("修改文件"):
        !strcmp(name,"write_file")?tr("写入文件"):!strcmp(name,"run_shell")?tr("执行命令"):
        !strcmp(name,"desktop_snapshot")?tr("读取桌面"):tr("操作桌面");
    const char *target=*sget(args,"path")?sget(args,"path"):!strcmp(name,"run_shell")?sget(args,"command"):sget(args,"app");
    const int width=std::max(24,terminal_columns()-6);
    std::string head=std::string(ui_bullet())+" "+label;
    if(*target){
        std::string inner=ui_clip(target,std::max(8,width-ui_width(head)-2));
        head+="("+inner+")";
    }
    printf("%s\n",ui_paint(Tone::Accent,ui_clip(head,width)).c_str());
    if(!strcmp(name,"search_text"))printf(tr("  查找：%.256s\n"),sget(args,"query"));
    fflush(stdout);
}
static void show_result(const char *name,J *r,double elapsed){
    bool failed=tool_failed(r);
    std::string detail;
    char text[512];
    if(*sget(r,"error")){detail=" · ";detail.append(sget(r,"error"));}
    else if(!strcmp(name,"run_shell")){
        snprintf(text,sizeof text,tr(" · 退出码 %d%s"),iget(r,"exit_code",0),bget(r,"timed_out")?tr(" · 超时"):"");
        detail=text;
    }
    else if(get(r,"written"))detail=tr(" · 已回读核对");
    else if(!strcmp(name,"read_file")){
        snprintf(text,sizeof text,tr(" · %d 字节"),json_object_get_string_len(get(r,"content")));
        detail=text;
    }
    else if(!strcmp(name,"search_text")){
        snprintf(text,sizeof text,tr(" · %zu 条匹配%s"),alen(get(r,"matches")),bget(r,"truncated")?tr(" · 结果有截断"):"");
        detail=text;
    }
    else if(!strcmp(name,"list_files")){
        snprintf(text,sizeof text,tr(" · %zu 项"),alen(get(r,"entries")));
        detail=text;
    }
    else if(get(r,"accepted"))detail=tr(" · 等待重新读取核对");
    char timing[48];
    snprintf(timing,sizeof timing," · %.1fs",elapsed);
    std::string line=std::string("  ")+ui_result()+"  ";
    line+=failed?tr("失败"):tr("完成");
    line+=detail;
    line+=timing;
    printf("%s\n",ui_paint(failed?Tone::Error:Tone::Dim,ui_clip(line,std::max(24,terminal_columns()-6))).c_str());
    if(!strcmp(name,"run_shell")&&*sget(r,"output")){
        const char *output=sget(r,"output");
        printf("%.*s\n",1600,output);
        if(strlen(output)>1600||bget(r,"truncated"))puts(tr("  …输出已截断，模型可见工具返回的输出片段。"));
    }
}
static const char *activity(const char *name){
    if(!strncmp(name,"mcp_",4))return tr("正在调用 MCP 工具…");
    if(!strcmp(name,"read_file"))return tr("正在读取文件…");
    if(!strcmp(name,"list_files"))return tr("正在查看目录…");
    if(!strcmp(name,"search_text"))return tr("正在搜索…");
    if(!strcmp(name,"write_file")||!strcmp(name,"edit_file"))return tr("正在编辑文件…");
    if(!strcmp(name,"run_shell"))return tr("正在执行命令…");
    return tr("正在操作桌面…");
}
#ifndef _WIN32
// Queued typing stays in the terminal input buffer while progress is drawn.
// An approval prompt temporarily lets Readline render its own input line.
struct OperationEcho {
    struct termios saved{};bool changed=false;
    explicit OperationEcho(const App &app){
        if(app.task_events<0&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)&&!tcgetattr(STDIN_FILENO,&saved)){
            struct termios mode=saved;mode.c_lflag&=~ECHO;
#ifdef IUTF8
            mode.c_iflag|=IUTF8;
#endif
            changed=!tcsetattr(STDIN_FILENO,TCSANOW,&mode);
        }
    }
    ~OperationEcho(){if(changed)tcsetattr(STDIN_FILENO,TCSANOW,&saved);}
};
#endif
class ThinkingProgress {
    App &app;
public:
    explicit ThinkingProgress(App &value):app(value){
        app.thinking=true;app.thinking_started=monotime();task_event(app,'t');
    }
    ~ThinkingProgress(){app.thinking=false;task_event(app,'e');progress_clear(&app);}
};
int run_task(App *a,const char *task){
#ifndef _WIN32
    OperationEcho input_echo(*a);
#endif
    std::unique_ptr<ResourceLock> desktop_lock;
    if(a->desktop){
        desktop_lock=std::make_unique<ResourceLock>(*a,true);
        if(!desktop_lock->acquired()){seterr(a,tr("桌面资源等待已取消、被占用或无法加锁；请返回对应对话处理已有操作。"));a->state=interrupted?"已取消":"失败";return -1;}
    }
    if(!a->messages)reset_session(a);
    size_t history_start=alen(a->messages);
    J *user=obj();
    put(user,"role",str("user"));
    put(user,"content",str(task));
    add(a->messages,user);
    history_checkpoint(*a,history_start);
    a->state="执行中";
    a->error[0]=0;
    a->task_tokens=0;
    const double task_started=monotime();
    if(!mcp_prepare(*a)){a->state="失败";return -1;}
    JsonPtr tool_owner(schemas(a));
    J *tools=tool_owner.get();
    std::vector<FailedOperation> unresolved;
    std::vector<Observation> seen;
    int failures=0,rc=-1;
    bool trimmed=false,desktop_unverified=false;
    const char *reply_marker=a->conversation_id.empty()?"":ui_assistant_marker(ui_color());
    for(int step=1;step<=a->max_steps&&!interrupted;step++){
        J *history=context(a,tools);
        if(!history)break;
        if(!trimmed&&alen(history)<alen(a->messages)){
            progress_clear(a);
            puts(tr("较早的任务未发送给模型，当前任务及其工具结果完整保留。"));
            trimmed=true;
        }
        J *reply=nullptr;
        bool streamed=false;
        {
            ThinkingProgress thinking(*a);
            if(a->verbose)printf(tr("[%d/%d] 正在请求模型…\n"),step,a->max_steps);
            else progress(a,tr("正在思考…"));
            fflush(stdout);
            // Print text as it arrives; the first chunk clears the "thinking"
            // row and opens the reply so the transcript fills in live.
            a->stream_text=[a,&streamed,reply_marker](const std::string &part){
                if(!streamed){
                    streamed=true;
                    progress_clear(a);
                    putchar('\n');
                    fputs(reply_marker,stdout);
                }
                fputs(part.c_str(),stdout);
                fflush(stdout);
            };
            reply=api_complete(a,a->config,history,tools);
            a->stream_text=nullptr;
        }
        json_object_put(history);
        if(!reply)break;
        J *calls=get(reply,"tool_calls");
        if(validate_calls(a,calls)){
            json_object_put(reply);
            break;
        }
        if(!alen(calls)){
            if(!unresolved.empty()||desktop_unverified){
                seterr(a,desktop_unverified?tr("桌面操作尚未重新读取核对，任务未确认完成。"):tr("仍有未恢复的操作失败，任务未确认完成；可继续描述需求或要求诊断。"));
                json_object_put(reply);
                break;
            }
            if(!*sget(reply,"content")){
                seterr(a,tr("模型未返回操作或回答。"));
                json_object_put(reply);
                break;
            }
            progress_clear(a);
            if(streamed)putchar('\n');
            else printf("%s%s\n",reply_marker,sget(reply,"content"));
            add(a->messages,reply);
            a->state="本轮结束";
            rc=0;
            break;
        }
        add(a->messages,reply);
        if(streamed)putchar('\n');
        else if(a->verbose&&*sget(reply,"content"))printf("%s%s\n",reply_marker,sget(reply,"content"));
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            if(interrupted){
                pending(a,calls,i);
                goto done;
            }
            const char *name=sget(fn,"name");
            J *args=parse_tool_arguments(sget(fn,"arguments"));
            if(a->verbose)show_call(name,args);
            else progress(a,activity(name));
            double tool_started=monotime();
            J *result=tool_call(a,name,args);
            a->progress_line=false;
            bool denied=bget(result,"denied");
            bool failed=tool_failed(result),loop=repeated(seen,name,args,result);
            recovery(a,unresolved,name,args,failed);
            a->error[0]=0;
            if(failed)failures++;
            else if(unresolved.empty())failures=0;
            if(!failed&&!strncmp(name,"desktop_",8)){
                if(mutation(name))desktop_unverified=true;
                else if(*sget(args,"app"))desktop_unverified=false;
            }
            if(a->verbose)show_result(name,result,monotime()-tool_started);
            else if(failed){
                progress_clear(a);
                if(*sget(result,"error"))fprintf(stderr,"%s\n",sget(result,"error"));
                else fprintf(stderr,tr("操作失败（退出码 %d）。\n"),iget(result,"exit_code",-1));
            }
            tool_result(a,sget(call,"id"),result);
            json_object_put(args);
            json_object_put(result);
            if(denied||interrupted||failures>=3||loop){
                pending(a,calls,i+1);
                if(denied)a->state="已取消";
                seterr(a,denied?tr("操作已取消。"):interrupted?tr("任务已停止。"):loop?tr("相同工具调用三次返回相同结果，已停止重复操作；可补充信息后继续。"):tr("三次操作失败且尚未恢复，已停止。"));
                goto done;
            }
        }
    }
    done:
    progress_clear(a);
    if(!a->error[0]&&rc)seterr(a,interrupted?tr("任务已停止。"):tr("达到步骤上限，任务未确认完成。"));
    if(interrupted)a->state="已取消";
    else if(rc&&strcmp(a->state,"已取消"))a->state="失败";
    {
        // One closing line per turn: how long it worked, tokens used, outcome.
        const char *outcome=rc==0?tr("完成"):interrupted||!strcmp(a->state,"已取消")?tr("已停止"):tr("未完成");
        std::string line="  "+std::string(tr("用时"))+" "+elapsed_label(monotime()-task_started);
        if(a->task_tokens){
            char tokens[64];
            snprintf(tokens,sizeof tokens,tr(" · %s 词元"),token_label(a->task_tokens).c_str());
            line+=tokens;
        }
        line+=" · ";line+=outcome;
        printf("\n%s\n",ui_paint(rc==0?Tone::Dim:Tone::Warning,line).c_str());
        fflush(stdout);
    }
    history_checkpoint(*a,history_start+1);
    if(a->record)save_record(a);
    return rc;
}
void save_record(App *a){
    char base[PATH_MAX],directory[PATH_MAX],path[PATH_MAX],id[33],leaf[48];
    std::string state_path=state_base();
    if(state_path.size()>=sizeof base)return;
    strcpy(base,state_path.c_str());
    if(path_suffix(directory,base,"/desk-agent/sessions"))return;
    id_string(id);
    snprintf(leaf,sizeof leaf,"/%s.json",id);
    if(path_suffix(path,directory,leaf))return;
    J *record=obj();
    put(record,"version",str(VERSION));
    put(record,"state",str(a->state));
    put(record,"messages",copy(a->messages));
    const char *data=json_object_to_json_string_ext(record,JSON_C_TO_STRING_PRETTY);
    if(!mkdirs(directory,0700)&&!atomic_file(path,data,strlen(data),0600))printf(tr("会话记录：%s（分享前检查隐私内容）\n"),path);
    else fputs(tr("无法保存会话记录。\n"),stderr);
    json_object_put(record);
}
