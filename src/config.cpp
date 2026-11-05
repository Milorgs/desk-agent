#include "desk.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static char *trim(char *s){
    while(isspace((unsigned char)*s))s++;
    char *e=s+strlen(s);
    while(e>s&&isspace((unsigned char)e[-1]))*--e=0;
    return s;
}
static J *parse_value(const char *s){
    struct json_tokener *tok=json_tokener_new();
    size_t length=strlen(s);
    J *v=json_tokener_parse_ex(tok,s,(int)length+1);
    bool ok=json_tokener_get_error(tok)==json_tokener_success;
    size_t end=json_tokener_get_parse_end(tok);
    if(end>length)end=length;
    while(isspace((unsigned char)s[end]))end++;
    if(s[end]&&s[end]!='#')ok=false;
    json_tokener_free(tok);
    if(!ok){
        json_object_put(v);
        return NULL;
    }
    return v;
}
J *config_default(const char *name){
    J *c=obj();
    put(c,"profile",str(name));
    put(c,"base_url",str("https://api.openai.com/v1"));
    put(c,"model",str(""));
    put(c,"api_key",str(""));
    put(c,"protocol",str("openai"));
    put(c,"max_tokens",json_object_new_int(4096));
    put(c,"anthropic_version",str("2023-06-01"));
    return c;
}
int config_name_valid(const char *name){
    size_t n=strlen(name);
    if(!n||n>160)return 0;
    int count=0;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++){
        if(*p<128&&!isalnum(*p)&&*p!='_'&&*p!='-')return 0;
        if((*p&0xc0)!=0x80)count++;
    }
    return count<=40;
}
static int normalize(App *a,J *c){
    const char *raw=sget(c,"base_url"),*protocol=sget(c,"protocol");
    char *url=strdup(raw);
    if(!url)return -1;
    size_t n=strlen(url);
    while(n&&isspace((unsigned char)url[n-1]))url[--n]=0;
    char *start=url;
    while(isspace((unsigned char)*start))start++;
    const char *suffix=!strcmp(protocol,"anthropic")?"/messages":"/chat/completions";
    n=strlen(start);
    while(n&&start[n-1]=='/')start[--n]=0;
    size_t k=strlen(suffix);
    if(n>=k&&!strcmp(start+n-k,suffix))start[n-=k]=0;
    CURLU *u=curl_url();
    CURLUcode rc=curl_url_set(u,CURLUPART_URL,start,0);
    char *scheme=NULL,*host=NULL,*part=NULL;
    bool valid=rc==CURLUE_OK;
    if(valid){
        curl_url_get(u,CURLUPART_SCHEME,&scheme,0);
        curl_url_get(u,CURLUPART_HOST,&host,0);
    }
    if(!scheme||!host)valid=false;
    if(valid&&strcmp(scheme,"https")&&!( !strcmp(scheme,"http")&&(!strcmp(host,"localhost")||!strcmp(host,"127.0.0.1")||!strcmp(host,"[::1]"))))valid=false;
    CURLUPart forbidden[]={
        CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT
    };
    for(size_t i=0;valid&&i<4;i++){
        if(curl_url_get(u,forbidden[i],&part,0)==CURLUE_OK){
            valid=false;
            curl_free(part);
            part=NULL;
        }
    }
    char *path=NULL;
    if(valid)curl_url_get(u,CURLUPART_PATH,&path,0);
    if(valid&&path&&(!strcmp(path+strlen(path)-(strlen(path)>=9?9:0),"/messages")||strstr(path,"/chat/completions")||strstr(path,"/responses")))valid=false;
    if(valid){
        char normalized[4096];
        if(!strcmp(protocol,"anthropic")&&(!path||!strcmp(path,"/")))snprintf(normalized,sizeof normalized,"%s/v1",start);
        else snprintf(normalized,sizeof normalized,"%s",start);
        if(strlen(start)>3900)valid=false;
        else put(c,"base_url",str(normalized));
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    curl_url_cleanup(u);
    free(url);
    if(!valid){
        seterr(a,tr("API 地址无效：使用 HTTPS（本机可 HTTP），并确保地址与接口类型匹配。禁止地址携带凭据或查询参数。"));
        return -1;
    }
    return 0;
}
int config_valid(App *a,J *c){
    if(!config_name_valid(sget(c,"profile"))){
        seterr(a,tr("配置名称不能为空，最多 40 字，不含空格或路径符号。"));
        return -1;
    }
    const char *protocol=sget(c,"protocol");
    if(strcmp(protocol,"openai")&&strcmp(protocol,"anthropic")){
        seterr(a,tr("接口类型应为 openai 或 anthropic。"));
        return -1;
    }
    if(!*sget(c,"model")||!*sget(c,"api_key")){
        seterr(a,tr("请先添加模型并填写 API Key。"));
        return -1;
    }
    const char *key=sget(c,"api_key");
    for(;*key;key++)if((unsigned char)*key<32||*key==127){
        seterr(a,tr("密钥包含无效字符。"));
        return -1;
    }
    int tokens=iget(c,"max_tokens",4096);
    if(tokens<1||tokens>65536){
        seterr(a,tr("max_tokens 必须为 1–65536。"));
        return -1;
    }
    const char *version=sget(c,"anthropic_version");
    if(strlen(version)!=10||version[4]!='-'||version[7]!='-'){
        seterr(a,tr("API 版本格式应为 YYYY-MM-DD。"));
        return -1;
    }
    for(int i=0;i<10;i++)if(i!=4&&i!=7&&!isdigit((unsigned char)version[i])){
        seterr(a,tr("API 版本格式无效。"));
        return -1;
    }
    return normalize(a,c);
}
static J *legacy_store(App *a){
    char path[PATH_MAX];
    if(path_suffix(path,a->config_dir,"/config.toml"))return NULL;
    size_t length=0;
    char *text=read_bytes(path,262144,&length);
    J *store=obj(),*profiles=obj();
    put(store,"profiles",profiles);
    put(store,"active_profile",str("default"));
    J *entry=NULL;
    if(!text){
        if(errno!=ENOENT){
            json_object_put(store);
            seterr(a,tr("无法读取旧配置。"));
            return NULL;
        }
        return store;
    }
    char *save=NULL;
    for(char *line=strtok_r(text,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
        char *s=trim(line);
        if(!*s||*s=='#')continue;
        if(*s=='['){
            entry=NULL;
            if(!strcmp(s,"[model]")){
                entry=config_default("default");
                put(profiles,"default",entry);
            }
            else if(!strncmp(s,"[profiles.",10)){
                char *end=strrchr(s,']');
                if(!end)goto bad;
                *end=0;
                J *name=parse_value(s+10);
                if(!name||!json_object_is_type(name,json_type_string)){
                    json_object_put(name);
                    goto bad;
                }
                const char *n=json_object_get_string(name);
                entry=config_default(n);
                put(profiles,n,entry);
                json_object_put(name);
            }
            else goto bad;
            continue;
        }
        char *equals=strchr(s,'=');
        if(!equals)goto bad;
        *equals=0;
        char *key=trim(s);
        J *value=parse_value(trim(equals+1));
        if(!value)goto bad;
        if(entry){
            put(entry,!strcmp(key,"name")?"model":key,value);
        }
        else if(!strcmp(key,"active_profile")){
            put(store,key,value);
        }
        else{
            json_object_put(value);
            goto bad;
        }
    }
    free(text);
    {
    if(path_suffix(path,a->config_dir,"/auth.json")){
        json_object_put(store);
        return NULL;
    }
    char *auth_text=read_bytes(path,262144,NULL);
    J *auth=NULL;
    if(auth_text){
        auth=json_tokener_parse(auth_text);
        free(auth_text);
        if(!auth||!json_object_is_type(auth,json_type_object)){
            json_object_put(auth);
            json_object_put(store);
            seterr(a,tr("旧密钥文件无效，未修改原文件。"));
            return NULL;
        }
    }
    else if(errno!=ENOENT){
        json_object_put(store);
        seterr(a,tr("无法读取旧密钥文件。"));
        return NULL;
    }
    json_object_object_foreach(profiles,name,c){
        const char *key=sget(get(auth,"profiles"),name);
        if(!*key&&!strcmp(name,"default"))key=sget(auth,"api_key");
        put(c,"api_key",str(key));
    }
    json_object_put(auth);
    return store;
    }
    bad: free(text);
    json_object_put(store);
    seterr(a,tr("旧 TOML 配置包含无法迁移的格式，原文件未改动。请检查配置格式。"));
    return NULL;
}
int store_load(App *a){
    std::string base_path=config_base();
    const char *base=base_path.c_str();
    if(snprintf(a->config_dir,sizeof a->config_dir,"%s/desk-agent",base)>=(int)sizeof a->config_dir){
        seterr(a,tr("配置目录路径过长。"));
        return -1;
    }
    char path[PATH_MAX];
    if(path_suffix(path,a->config_dir,"/settings.json")){
        seterr(a,tr("配置目录路径过长。"));
        return -1;
    }
    char *text=read_bytes(path,524288,NULL);
    if(text){
        a->store=json_tokener_parse(text);
        free(text);
        if(!a->store||!json_object_is_type(get(a->store,"profiles"),json_type_object)||!json_object_is_type(get(a->store,"active_profile"),json_type_string)){
            seterr(a,tr("settings.json 格式无效。"));
            return -1;
        }
    }
    else{
        if(errno!=ENOENT){
            seterr(a,tr("无法读取配置文件。"));
            return -1;
        }
        a->store=legacy_store(a);
        if(!a->store)return -1;
    }
    const char *active=sget(a->store,"active_profile");
    J *saved=get(get(a->store,"profiles"),active);
    a->config=saved?copy(saved):config_default("default");
    const char *fields[]={
        "base_url","model","api_key","protocol"
    }
    ,*envs[]={
        "DESK_BASE_URL","DESK_MODEL","DESK_API_KEY","DESK_API_TYPE"
    };
    for(int i=0;i<4;i++){
        const char *v=env_value(envs[i]);
        if(v&&*v)put(a->config,fields[i],str(v));
    }
    return 0;
}
int save_config(App *a,J *c){
    if(config_valid(a,c))return -1;
    J *next=copy(a->store);
    put(next,"active_profile",str(sget(c,"profile")));
    put(get(next,"profiles"),sget(c,"profile"),copy(c));
    if(mkdirs(a->config_dir,0700)){
        json_object_put(next);
        seterr(a,tr("无法创建私有配置目录。"));
        return -1;
    }
    char path[PATH_MAX];
    if(path_suffix(path,a->config_dir,"/settings.json")){
        json_object_put(next);
        return -1;
    }
    const char *data=json_object_to_json_string_ext(next,JSON_C_TO_STRING_PRETTY);
    if(atomic_file(path,data,strlen(data),0600)){
        json_object_put(next);
        seterr(a,tr("保存配置失败，原设置未替换。"));
        return -1;
    }
    J *selected=copy(c);
    json_object_put(a->store);
    a->store=next;
    json_object_put(a->config);
    a->config=selected;
    desktop_clear(a);
    reset_session(a);
    return 0;
}
int activate(App *a,const char *name){
    J *entry=get(get(a->store,"profiles"),name);
    if(!entry){
        seterr(a,tr("没有这个模型配置。"));
        return -1;
    }
    J *candidate=copy(entry);
    int rc=save_config(a,candidate);
    json_object_put(candidate);
    return rc;
}
