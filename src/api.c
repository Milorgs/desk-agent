#include "desk.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct{
    char *data;
    size_t len;
    bool oversized;
}
Buffer;
static size_t receive(char *p,size_t size,size_t count,void *user){
    Buffer *b=user;
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    if(n>RESPONSE_LIMIT-b->len){
        b->oversized=true;
        return 0;
    }
    char *next=realloc(b->data,b->len+n+1);
    if(!next)return 0;
    b->data=next;
    memcpy(b->data+b->len,p,n);
    b->len+=n;
    b->data[b->len]=0;
    return n;
}
static int progress(void *u,curl_off_t d,curl_off_t n,curl_off_t t,curl_off_t v){
    (void)u;
    (void)d;
    (void)n;
    (void)t;
    (void)v;
    return interrupted?1:0;
}
J *api_request(App *a,J *c,const char *endpoint,J *payload){
    if(config_valid(a,c))return NULL;
    char url[8192];
    if(snprintf(url,sizeof url,"%s%s",sget(c,"base_url"),endpoint)>=(int)sizeof url){
        seterr(a,"API 地址过长。");
        return NULL;
    }
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    const char *body=payload?json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN):NULL;
    for(int attempt=0;attempt<3;attempt++){
        CURL *h=curl_easy_init();
        if(!h){
            seterr(a,"无法初始化 HTTP。");
            return NULL;
        }
        Buffer b={
            0
        };
        struct curl_slist *headers=NULL;
        headers=curl_slist_append(headers,"Accept: application/json");
        headers=curl_slist_append(headers,"Content-Type: application/json");
        char *auth=NULL;
        asprintf(&auth,native?"x-api-key: %s":"Authorization: Bearer %s",sget(c,"api_key"));
        headers=curl_slist_append(headers,auth);
        free(auth);
        if(native){
            char version[128];
            snprintf(version,sizeof version,"anthropic-version: %s",sget(c,"anthropic_version"));
            headers=curl_slist_append(headers,version);
        }
        curl_easy_setopt(h,CURLOPT_URL,url);
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,10L);
        curl_easy_setopt(h,CURLOPT_TIMEOUT,60L);
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
        curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);
        curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,progress);
        curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
        if(body){
            curl_easy_setopt(h,CURLOPT_POSTFIELDS,body);
            curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE,(long)strlen(body));
        }
        CURLcode rc=curl_easy_perform(h);
        long status=0;
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(h);
        if(rc!=CURLE_OK){
            free(b.data);
            seterr(a,interrupted?"请求已取消。":b.oversized?"API 响应超过大小限制。":"API 连接失败或超时；未自动重发请求。");
            return NULL;
        }
        if(status>=200&&status<300){
            J *data=b.data?json_tokener_parse(b.data):NULL;
            free(b.data);
            if(!data||!json_object_is_type(data,json_type_object)){
                json_object_put(data);
                seterr(a,"API 未返回有效 JSON 对象，请检查接口地址。");
                return NULL;
            }
            return data;
        }
        free(b.data);
        bool retry=status==429||status==500||status==502||status==503||status==504||status==529;
        if(retry&&attempt<2&&!interrupted){
            fprintf(stderr,"API HTTP %ld，正在重试（%d/2）…\n",status,attempt+1);
            struct timespec t={
                1<<attempt,0
            };
            while(nanosleep(&t,&t)&&!interrupted){
            }
            if(interrupted){
                seterr(a,"请求已取消。");
                return NULL;
            }
            continue;
        }
        const char *help=status==401?"密钥无效或已过期。":status==403?"没有模型或接口权限。":status==404?"地址或模型不存在。":status==429?"限流或额度不足。":status==400?"接口类型或参数不兼容。":"服务商请求失败。";
        snprintf(a->error,sizeof a->error,"API HTTP %ld：%s",status,help);
        return NULL;
    }
    return NULL;
}
static void block_message(J *history,const char *role,J *blocks){
    size_t n=alen(history);
    J *last=n?at(history,n-1):NULL;
    if(last&&!strcmp(sget(last,"role"),role)){
        J *content=get(last,"content");
        for(size_t i=0;i<alen(blocks);i++)add(content,copy(at(blocks,i)));
        json_object_put(blocks);
    }
    else{
        J *m=obj();
        put(m,"role",str(role));
        put(m,"content",blocks);
        add(history,m);
    }
}
static J *textblock(const char *text){
    J *b=obj();
    put(b,"type",str("text"));
    put(b,"text",str(text));
    return b;
}
J *native_payload(J *c,J *messages,J *tools){
    J *body=obj(),*history=arr();
    put(body,"model",str(sget(c,"model")));
    put(body,"max_tokens",json_object_new_int(iget(c,"max_tokens",4096)));
    put(body,"messages",history);
    for(size_t i=0;i<alen(messages);i++){
        J *m=at(messages,i);
        const char *role=sget(m,"role");
        if(!strcmp(role,"system")){
            put(body,"system",copy(get(m,"content")));
            continue;
        }
        J *blocks=arr();
        if(!strcmp(role,"user"))add(blocks,textblock(sget(m,"content")));
        else if(!strcmp(role,"tool")){
            J *b=obj();
            put(b,"type",str("tool_result"));
            put(b,"tool_use_id",str(sget(m,"tool_call_id")));
            put(b,"content",str(sget(m,"content")));
            J *result=json_tokener_parse(sget(m,"content"));
            put(b,"is_error",json_object_new_boolean(get(result,"error")||bget(result,"denied")));
            json_object_put(result);
            add(blocks,b);
            role="user";
        }
        else if(!strcmp(role,"assistant")){
            J *raw=get(m,"_anthropic_content");
            if(raw){
                json_object_put(blocks);
                blocks=copy(raw);
            }
            else{
                if(*sget(m,"content"))add(blocks,textblock(sget(m,"content")));
                J *calls=get(m,"tool_calls");
                for(size_t k=0;k<alen(calls);k++){
                    J *call=at(calls,k),*fn=get(call,"function"),*b=obj();
                    put(b,"type",str("tool_use"));
                    put(b,"id",str(sget(call,"id")));
                    put(b,"name",str(sget(fn,"name")));
                    J *args=json_tokener_parse(sget(fn,"arguments"));
                    put(b,"input",args?args:obj());
                    add(blocks,b);
                }
            }
        }
        if(alen(blocks))block_message(history,role,blocks);
        else json_object_put(blocks);
    }
    if(alen(tools)){
        J *defs=arr();
        for(size_t i=0;i<alen(tools);i++){
            J *f=get(at(tools,i),"function"),*d=obj();
            put(d,"name",str(sget(f,"name")));
            put(d,"description",str(sget(f,"description")));
            put(d,"input_schema",copy(get(f,"parameters")));
            add(defs,d);
        }
        put(body,"tools",defs);
        J *choice=obj();
        put(choice,"type",str("auto"));
        put(body,"tool_choice",choice);
    }
    return body;
}
J *native_response(App *a,J *data){
    const char *stop=sget(data,"stop_reason");
    if(!strcmp(stop,"max_tokens")||!strcmp(stop,"model_context_window_exceeded")){
        seterr(a,"模型响应被截断，本轮工具未执行。");
        return NULL;
    }
    if(strcmp(stop,"end_turn")&&strcmp(stop,"tool_use")&&strcmp(stop,"stop_sequence")){
        seterr(a,"模型拒绝或尚未结束响应，本轮工具未执行。");
        return NULL;
    }
    J *blocks=get(data,"content");
    if(!json_object_is_type(blocks,json_type_array)){
        seterr(a,"Anthropic 响应缺少 content 数组。");
        return NULL;
    }
    J *result=obj(),*calls=arr();
    char *text=calloc(1,RESPONSE_LIMIT+1);
    if(!text){
        json_object_put(result);
        json_object_put(calls);
        return NULL;
    }
    for(size_t i=0;i<alen(blocks);i++){
        J *b=at(blocks,i);
        const char *type=sget(b,"type");
        if(!strcmp(type,"text")){
            const char *part=sget(b,"text");
            if(strlen(text)+strlen(part)+2>RESPONSE_LIMIT)goto bad;
            if(*text)strcat(text,"\n");
            strcat(text,part);
        }
        else if(!strcmp(type,"tool_use")){
            const char *id=sget(b,"id"),*name=sget(b,"name");
            if(!*id||!*name||!json_object_is_type(get(b,"input"),json_type_object))goto bad;
            for(size_t k=0;k<alen(calls);k++)if(!strcmp(sget(at(calls,k),"id"),id))goto bad;
            J *call=obj(),*f=obj();
            put(call,"id",str(id));
            put(call,"type",str("function"));
            put(f,"name",str(name));
            put(f,"arguments",str(json_object_to_json_string_ext(get(b,"input"),JSON_C_TO_STRING_PLAIN)));
            put(call,"function",f);
            add(calls,call);
        }
        else if(strcmp(type,"thinking")&&strcmp(type,"redacted_thinking"))goto bad;
    }
    if((!strcmp(stop,"tool_use"))!=(alen(calls)>0))goto bad;
    put(result,"role",str("assistant"));
    put(result,"content",*text?str(text):NULL);
    put(result,"_anthropic_content",copy(blocks));
    if(alen(calls))put(result,"tool_calls",calls);
    else json_object_put(calls);
    free(text);
    return result;
    bad:free(text);
    json_object_put(result);
    json_object_put(calls);
    seterr(a,"Anthropic 工具或内容块格式无效，本轮工具未执行。");
    return NULL;
}
J *api_complete(App *a,J *c,J *messages,J *tools){
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    J *body;
    if(native)body=native_payload(c,messages,tools);
    else{
        body=obj();
        put(body,"model",str(sget(c,"model")));
        put(body,"messages",copy(messages));
        if(alen(tools)){
            put(body,"tools",copy(tools));
            put(body,"tool_choice",str("auto"));
        }
    }
    J *data=api_request(a,c,native?"/messages":"/chat/completions",body);
    json_object_put(body);
    if(!data)return NULL;
    J *result=NULL;
    if(native)result=native_response(a,data);
    else{
        J *choice=at(get(data,"choices"),0);
        if(!strcmp(sget(choice,"finish_reason"),"length"))seterr(a,"模型响应被截断，本轮工具未执行。");
        else if(get(get(choice,"message"),"refusal"))seterr(a,"模型拒绝处理本次请求。");
        else if(json_object_is_type(get(choice,"message"),json_type_object))result=copy(get(choice,"message"));
        else seterr(a,"Chat Completions 响应格式无效。");
    }
    json_object_put(data);
    return result;
}
J *api_models(App *a,J *c){
    J *data=api_request(a,c,!strcmp(sget(c,"protocol"),"anthropic")?"/models?limit=1000":"/models",NULL);
    if(!data)return NULL;
    J *entries=get(data,"data"),*out=arr();
    if(!json_object_is_type(entries,json_type_array)){
        seterr(a,"服务商不支持模型列表；可手动填写模型名称。");
        json_object_put(out);
        out=NULL;
    }
    else for(size_t i=0;i<alen(entries)&&i<500;i++){
        const char *id=sget(at(entries,i),"id");
        if(*id)add(out,str(id));
    }
    json_object_put(data);
    return out;
}
int api_check(App *a,J *c){
    J *messages=arr(),*m=obj();
    put(m,"role",str("user"));
    put(m,"content",str("Call connection_check with empty arguments."));
    add(messages,m);
    J *tools=arr(),*t=obj(),*f=obj(),*parameters=obj();
    put(t,"type",str("function"));
    put(f,"name",str("connection_check"));
    put(f,"description",str("Check tool support without operating the computer."));
    put(parameters,"type",str("object"));
    put(parameters,"properties",obj());
    put(parameters,"required",arr());
    put(parameters,"additionalProperties",json_object_new_boolean(0));
    put(f,"parameters",parameters);
    put(t,"function",f);
    add(tools,t);
    J *reply=api_complete(a,c,messages,tools);
    json_object_put(messages);
    json_object_put(tools);
    if(!reply)return -1;
    int ok=-1;
    J *calls=get(reply,"tool_calls");
    for(size_t i=0;i<alen(calls);i++){
        J *call=at(calls,i),*fn=get(call,"function"),*args=json_tokener_parse(sget(fn,"arguments"));
        if(*sget(call,"id")&&!strcmp(sget(fn,"name"),"connection_check")&&json_object_is_type(args,json_type_object)&&json_object_object_length(args)==0)ok=0;
        json_object_put(args);
    }
    json_object_put(reply);
    if(ok)seterr(a,"接口响应成功，但未验证工具调用能力；原配置未改动。");
    return ok;
}
