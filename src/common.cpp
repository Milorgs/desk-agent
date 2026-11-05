#include "desk.hpp"
#include "tasks.hpp"
#ifndef _WIN32
#include <unistd.h>
#endif
#include <openssl/rand.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>
std::atomic<int> interrupted{0};
std::atomic<bool> input_closed{false};
void progress_clear(App *app){
    if(app->progress_visible){
#ifdef _WIN32
        fputs("\r                              \r",stdout);
#else
        fputs("\r\033[2K",stdout);
#endif
        fflush(stdout);app->progress_visible=false;
    }
}
void progress(App *app,const char *text){
    progress_clear(app);
    if(app->task_events<0&&isatty(1)){fputs(text,stdout);app->progress_visible=true;}
    else puts(text);
    fflush(stdout);
}
J *obj(void){
    return json_object_new_object();
}
J *arr(void){
    return json_object_new_array();
}
J *str(const char *s){
    return json_object_new_string(s?s:"");
}
J *get(J *j,const char *key){
    J *v=NULL;
    if(j&&json_object_is_type(j,json_type_object))json_object_object_get_ex(j,key,&v);
    return v;
}
const char *sget(J *j,const char *key){
    J *v=get(j,key);
    return v&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";
}
int iget(J *j,const char *key,int fallback){
    J *v=get(j,key);
    return v&&json_object_is_type(v,json_type_int)?json_object_get_int(v):fallback;
}
bool bget(J *j,const char *key){
    return json_object_get_boolean(get(j,key));
}
bool tool_failed(J *r){
    J *accepted=get(r,"accepted");
    return get(r,"error")||bget(r,"denied")||bget(r,"timed_out")||iget(r,"exit_code",0)!=0||(accepted&&!json_object_get_boolean(accepted));
}
bool valid_utf8(std::string_view text) noexcept {
    for(size_t i=0;i<text.size();){
        auto c=static_cast<unsigned char>(text[i]);
        size_t width=c<0x80?1:(c>=0xc2&&c<=0xdf)?2:(c>=0xe0&&c<=0xef)?3:(c>=0xf0&&c<=0xf4)?4:0;
        if(!width||width>text.size()-i)return false;
        for(size_t k=1;k<width;k++)if((static_cast<unsigned char>(text[i+k])&0xc0)!=0x80)return false;
        if(width>=3){
            auto next=static_cast<unsigned char>(text[i+1]);
            if((c==0xe0&&next<0xa0)||(c==0xed&&next>=0xa0)||(c==0xf0&&next<0x90)||(c==0xf4&&next>=0x90))return false;
        }
        i+=width;
    }
    return true;
}
void put(J *j,const char *key,J *v){
    json_object_object_add(j,key,v);
}
void add(J *j,J *v){
    json_object_array_add(j,v);
}
J *copy(J *j){
    J *v=NULL;
    if(j&&json_object_deep_copy(j,&v,NULL)!=0)return NULL;
    return v;
}
J *err(const char *m){
    J *v=obj();
    put(v,"error",str(m));
    return v;
}
void seterr(App *a,const char *m){
    snprintf(a->error,sizeof a->error,"%s",m);
}
double monotime(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void id_string(char out[33]){unsigned char bytes[16];if(RAND_bytes(bytes,16)!=1)throw std::runtime_error("Random generation failed");for(size_t i=0;i<16;i++)snprintf(out+i*2,3,"%02x",bytes[i]);}
void app_cleanup(App *a){
    desktop_destroy(a);
    json_object_put(a->store);
    json_object_put(a->config);
    json_object_put(a->messages);
}
size_t alen(J *j){
    return j&&json_object_is_type(j,json_type_array)?json_object_array_length(j):0;
}
J *at(J *j,size_t index){
    return j&&json_object_is_type(j,json_type_array)?json_object_array_get_idx(j,index):NULL;
}
int path_suffix(char out[PATH_MAX],const char *base,const char *suffix){
    size_t n=strlen(base),m=strlen(suffix);
    if(n+m>=PATH_MAX){
        out[0]=0;
        return -1;
    }
    memcpy(out,base,n);
    memcpy(out+n,suffix,m+1);
    return 0;
}
const char *permission_name(const App &app){
    return tr(app.yes?"完全访问权限":app.auto_edit?"帮我批准":"请求批准");
}
const char *reasoning_name(const char *effort){
    if(!strcmp(effort,"none"))return tr("关闭");
    if(!strcmp(effort,"minimal"))return tr("最低");
    if(!strcmp(effort,"low"))return tr("低");
    if(!strcmp(effort,"medium"))return tr("中");
    if(!strcmp(effort,"high"))return tr("高");
    if(!strcmp(effort,"xhigh"))return tr("极高");
    if(!strcmp(effort,"max"))return tr("最大");
    return tr("服务商默认（未指定）");
}
bool approval_required(const App &app,Operation operation){
    return !app.yes&&!(app.auto_edit&&operation==Operation::FileEdit);
}
int approve(App *a,const char *description,Operation operation){
    if(interrupted)return 0;
    if(!approval_required(*a,operation)){if(a->verbose)printf("\n%s\n",description);return 1;}
    progress_clear(a);
    bool compact=!a->verbose&&a->task_events<0;
    if(compact){
        size_t count=strcspn(description,"\n");if(count>300)count=300;
        while(count&&(static_cast<unsigned char>(description[count])&0xc0)==0x80)count--;
        printf("\n%.*s%s\n",static_cast<int>(count),description,description[count]?"…":"");
    }else printf("\n%s\n",description);
    if(a->task_events<0&&!isatty(STDIN_FILENO))return 0;
    task_event(*a,'a');
    bool yes=false;
    for(;;){
        const char *prompt=a->task_events>=0?tr("等待你在任务菜单中确认此次操作。"):compact?tr("请求批准：[1] 允许 [2] 拒绝 [3] 查看完整操作 [0] 取消："):tr("允许此次操作？[1] 允许 [2] 拒绝 [0] 取消：");
        char *value=input(prompt,nullptr,false);
        if(value&&compact&&!strcmp(value,"3")){free(value);printf("\n%s\n",description);continue;}
        yes=value&&!strcmp(value,"1");free(value);break;
    }
    task_event(*a,'r');
    if(yes&&!a->verbose)progress(a,tr("正在执行操作…"));
    return yes;
}

J *load_tokens(const App &app){
    char path[PATH_MAX];
    if(path_suffix(path,app.config_dir,"/token-usage.json"))return nullptr;
    std::unique_ptr<char,decltype(&free)> data(read_bytes(path,1048576,nullptr),free);
    if(!data)return errno==ENOENT?obj():nullptr;
    JsonPtr usage(json_tokener_parse(data.get()));
    return json_object_is_type(usage.get(),json_type_object)?usage.release():nullptr;
}

std::uint64_t model_tokens(J *usage,J *config){
    return json_object_get_uint64(get(get(usage,sget(config,"profile")),sget(config,"model")));
}

bool record_tokens(App &app,J *config,std::uint64_t amount){
    if(!amount)return true;
    if(!*app.config_dir||mkdirs(app.config_dir,0700))return false;
    ResourceLock lock(app.config_dir);
    if(!lock.acquired())return false;
    JsonPtr usage(load_tokens(app));
    char path[PATH_MAX];
    if(!usage||path_suffix(path,app.config_dir,"/token-usage.json"))return false;
    const char *profile=sget(config,"profile");
    J *models=get(usage.get(),profile);
    if(!models){models=obj();put(usage.get(),profile,models);}
    if(!json_object_is_type(models,json_type_object))return false;
    auto total=add_tokens(model_tokens(usage.get(),config),amount);
    put(models,sget(config,"model"),json_object_new_uint64(total));
    const char *text=json_object_to_json_string_ext(usage.get(),JSON_C_TO_STRING_PLAIN);
    return strlen(text)<=1048576&&atomic_file(path,text,strlen(text),0600)==0;
}
