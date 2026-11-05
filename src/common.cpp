#include "desk.hpp"
#include "tasks.hpp"
#ifndef _WIN32
#include <unistd.h>
#endif
#include <openssl/rand.h>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <stdexcept>
std::atomic<int> interrupted{0};
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
int approve(App *a,const char *description){
    if(interrupted)return 0;
    printf("\n%s\n",description);
    if(a->yes)return 1;
    if(a->task_events<0&&!isatty(STDIN_FILENO))return 0;
    task_event(*a,'a');
    char *v=input(a->task_events>=0?tr("等待你在任务菜单中确认此次操作。"):tr("允许此次操作？[1] 允许 [2] 拒绝 [0] 取消："),NULL,false);
    task_event(*a,'r');
    int yes=v&&!strcmp(v,"1");
    free(v);
    return yes;
}
