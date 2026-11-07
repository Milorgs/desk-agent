#include "desk.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <thread>
#include <chrono>
struct Buffer {
    std::string data;
    bool oversized=false;
};
// Incremental reader for server-sent events. Both protocols are reduced to the
// same three outputs (text, tool calls, stop reason), so the existing response
// validation can run unchanged on the assembled result.
struct StreamState {
    std::string raw;
    std::string pending;
    std::string event_data;
    std::string text;
    std::vector<std::string> ids,names,args;
    std::string stop_reason;
    std::string usage;
    CURL *http=nullptr;
    bool native=false, events=false, complete=false, invalid=false, oversized=false;
    std::function<void(const std::string &)> delta;
};
static void stream_event(StreamState *st,const char *json){
    st->events=true;
    if(st->complete){st->invalid=true;return;}
    if(!st->native&&!strcmp(json,"[DONE]")){st->complete=true;return;}
    JsonPtr event(json_tokener_parse(json));
    J *o=event.get();
    if(!json_object_is_type(o,json_type_object)||get(o,"error")){st->invalid=true;return;}
    const char *type=sget(o,"type");
    if(st->native&&!strcmp(type,"message_stop")){st->complete=true;return;}
    if(!strcmp(type,"error")){st->invalid=true;return;}
    if(!strcmp(type,"content_block_start")){
        J *block=get(o,"content_block");
        if(!strcmp(sget(block,"type"),"tool_use")){
            st->ids.push_back(sget(block,"id"));
            st->names.push_back(sget(block,"name"));
            st->args.emplace_back();
        }
    }else if(!strcmp(type,"content_block_delta")){
        J *d=get(o,"delta");
        const char *part=sget(d,"text");
        if(*part){
            st->text+=part;
            if(st->delta)st->delta(part);
        }
        if(*sget(d,"partial_json")&&!st->args.empty())st->args.back()+=sget(d,"partial_json");
    }else if(!strcmp(type,"message_delta")){
        const char *stop=sget(get(o,"delta"),"stop_reason");
        if(*stop)st->stop_reason=stop;
        J *usage=get(o,"usage");
        if(json_object_is_type(usage,json_type_object))st->usage=json_object_to_json_string_ext(usage,JSON_C_TO_STRING_PLAIN);
    }else if(!strcmp(type,"message_start")){
        J *usage=get(get(o,"message"),"usage");
        if(json_object_is_type(usage,json_type_object))st->usage=json_object_to_json_string_ext(usage,JSON_C_TO_STRING_PLAIN);
    }else{
        J *choice=at(get(o,"choices"),0);
        J *d=get(choice,"delta");
        const char *part=sget(d,"content");
        if(*part){
            st->text+=part;
            if(st->delta)st->delta(part);
        }
        J *calls=get(d,"tool_calls");
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            int index=iget(call,"index",static_cast<int>(i));
            if(index<0||index>=12){st->invalid=true;return;}
            while(st->ids.size()<=static_cast<size_t>(index)){st->ids.emplace_back();st->names.emplace_back();st->args.emplace_back();}
            if(*sget(call,"id"))st->ids[index]=sget(call,"id");
            if(*sget(fn,"name"))st->names[index]+=sget(fn,"name");
            if(*sget(fn,"arguments"))st->args[index]+=sget(fn,"arguments");
        }
        const char *finish=sget(choice,"finish_reason");
        if(*finish)st->stop_reason=finish;
        J *usage=get(o,"usage");
        if(json_object_is_type(usage,json_type_object))st->usage=json_object_to_json_string_ext(usage,JSON_C_TO_STRING_PLAIN);
    }
}
static void stream_feed(StreamState *st,const char *bytes,size_t n){
    st->pending.append(bytes,n);
    size_t start=0;
    for(;;){
        auto end=st->pending.find('\n',start);
        if(end==std::string::npos)break;
        std::string line=st->pending.substr(start,end-start);
        start=end+1;
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty()&&!st->event_data.empty()){
            stream_event(st,st->event_data.c_str());
            st->event_data.clear();
        }
        if(!line.starts_with("data:"))continue;
        auto payload=line.substr(5);
        if(!payload.empty()&&payload.front()==' ')payload.erase(0,1);
        if(!st->event_data.empty())st->event_data+='\n';
        st->event_data+=payload;
    }
    st->pending.erase(0,start);
}
static size_t receive(char *p,size_t size,size_t count,void *user){
    auto *b=static_cast<Buffer *>(user);
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    if(n>RESPONSE_LIMIT-b->data.size()){
        b->oversized=true;
        return 0;
    }
    try { b->data.append(p,n); }
    catch(...) { return 0; } // Exceptions must not cross libcurl's C callback.
    return n;
}
static size_t stream_chunk(char *p,size_t size,size_t count,void *user){
    auto *st=static_cast<StreamState *>(user);
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    long status=0;
    curl_easy_getinfo(st->http,CURLINFO_RESPONSE_CODE,&status);
    if(status<200||status>=300)return n;
    if(n>RESPONSE_LIMIT-st->raw.size()){
        st->oversized=true;
        return 0;
    }
    try {
        st->raw.append(p,n);
        stream_feed(st,p,n);
    }
    catch(...) { return 0; }
    return n;
}
static int transfer_progress(void *u,curl_off_t d,curl_off_t n,curl_off_t t,curl_off_t v){
    if(u){
        auto *app=static_cast<App *>(u);
        check_exit(*app);
        progress_tick(app);
    }
    (void)d;
    (void)n;
    (void)t;
    (void)v;
    return interrupted?1:0;
}
J *api_request(App *a,J *c,const char *endpoint,J *payload,void *stream_void) try {
    auto *stream=static_cast<StreamState *>(stream_void);
    if(config_valid(a,c))return NULL;
    char url[8192];
    if(snprintf(url,sizeof url,"%s%s",sget(c,"base_url"),endpoint)>=(int)sizeof url){
        seterr(a,tr("API 地址过长。"));
        return NULL;
    }
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    const char *body=payload?json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN):NULL;
    if(!a->api_http)a->api_http.reset(curl_easy_init());
    if(!a->api_http){seterr(a,tr("无法初始化 HTTP。"));return nullptr;}
    for(int attempt=0;attempt<3;attempt++){
        CURL *h=a->api_http.get();
        Buffer b;
        HttpHeaders headers;
        headers.append(stream?"Accept: text/event-stream":"Accept: application/json");
        headers.append("Content-Type: application/json");
        headers.append(std::string(native?"x-api-key: ":"Authorization: Bearer ")+sget(c,"api_key"));
        if(native)headers.append(std::string("anthropic-version: ")+sget(c,"anthropic_version"));
        CurlRequest request(h);
        curl_easy_setopt(h,CURLOPT_URL,url);
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers.get());
        curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,10L);
        // Long generations stream for minutes; the connect timeout still
        // bounds a dead endpoint, and a stalled transfer aborts on low speed.
        curl_easy_setopt(h,CURLOPT_TIMEOUT,600L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_LIMIT,1L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_TIME,120L);
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        if(stream){
            stream->http=h;
            curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,stream_chunk);
            curl_easy_setopt(h,CURLOPT_WRITEDATA,stream);
        }else{
            curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
            curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);
        }
        curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,transfer_progress);
        curl_easy_setopt(h,CURLOPT_XFERINFODATA,a);
        curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
        if(body){
            curl_easy_setopt(h,CURLOPT_POSTFIELDS,body);
            curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE,(long)strlen(body));
        }
        CURLcode rc=curl_easy_perform(h);
        long status=0;
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
        if(rc!=CURLE_OK||interrupted){
            seterr(a,interrupted?tr("请求已取消。"):(stream?stream->oversized:b.oversized)?tr("API 响应超过大小限制。"):tr("API 连接失败或超时；未自动重发请求。"));
            return NULL;
        }
        if(status>=200&&status<300){
            const std::string &payload=stream?stream->raw:b.data;
            if(!valid_utf8(payload)){
                seterr(a,tr("API 返回的文本不是有效 UTF-8。"));
                return nullptr;
            }
            if(stream&&stream->events){
                if(stream->invalid||!stream->complete||stream->stop_reason.empty()||!stream->event_data.empty()){
                    seterr(a,tr("模型响应被截断，本轮工具未执行。"));
                    return nullptr;
                }
                return obj();
            }
            J *data=payload.empty()?nullptr:json_tokener_parse(payload.c_str());
            if(!data||!json_object_is_type(data,json_type_object)){
                json_object_put(data);
                seterr(a,tr("API 未返回有效 JSON 对象，请检查接口地址。"));
                return NULL;
            }
            return data;
        }
        bool retry=status==429||status==500||status==502||status==503||status==504||status==529;
        if(retry&&attempt<2&&!interrupted){
            char message[160];
            snprintf(message,sizeof message,tr("API HTTP %ld，正在重试（%d/2）…"),status,attempt+1);
            if(a->progress_visible)progress(a,message);
            else fprintf(stderr,"%s\n",message);
            auto until=monotime()+(1<<attempt);
            while(monotime()<until&&!interrupted){check_exit(*a);progress_tick(a);std::this_thread::sleep_for(std::chrono::milliseconds(50));}
            if(interrupted){
                seterr(a,tr("请求已取消。"));
                return NULL;
            }
            continue;
        }
        const char *help=status==401?tr("密钥无效或已过期。"):status==403?tr("没有模型或接口权限。"):status==404?tr("地址或模型不存在。"):status==429?tr("限流或额度不足。"):status==400?tr("接口类型或参数不兼容。"):tr("服务商请求失败。");
        if(status==400&&payload&&*sget(c,"reasoning_effort"))
            snprintf(a->error,sizeof a->error,"API HTTP 400：%s",tr("接口或模型可能不支持所选推理档位或工具组合；请调整档位或恢复服务商默认。未自动降档或更换模型。"));
        else snprintf(a->error,sizeof a->error,"API HTTP %ld：%s",status,help);
        return NULL;
    }
    return NULL;
}catch(const std::bad_alloc &){
    seterr(a,tr("无法分配请求头内存。"));
    return nullptr;
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
    const char *effort=sget(c,"reasoning_effort");
    if(*effort){
        J *thinking=obj();put(thinking,"type",str(!strcmp(effort,"none")?"disabled":"adaptive"));put(body,"thinking",thinking);
        if(strcmp(effort,"none")){J *output=obj();put(output,"effort",str(effort));put(body,"output_config",output);}
    }
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
            put(b,"is_error",json_object_new_boolean(tool_failed(result)));
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
        seterr(a,tr("模型响应被截断，本轮工具未执行。"));
        return NULL;
    }
    if(strcmp(stop,"end_turn")&&strcmp(stop,"tool_use")&&strcmp(stop,"stop_sequence")){
        seterr(a,tr("模型拒绝或尚未结束响应，本轮工具未执行。"));
        return NULL;
    }
    J *blocks=get(data,"content");
    if(!json_object_is_type(blocks,json_type_array)){
        seterr(a,tr("Anthropic 响应缺少 content 数组。"));
        return NULL;
    }
    JsonPtr result(obj()),calls(arr());
    std::string text;
    for(size_t i=0;i<alen(blocks);i++){
        J *b=at(blocks,i);
        const char *type=sget(b,"type");
        if(!strcmp(type,"text")){
            const char *part=sget(b,"text");
            if(text.size()+strlen(part)+2>RESPONSE_LIMIT)goto bad;
            if(!text.empty())text+='\n';
            text+=part;
        }
        else if(!strcmp(type,"tool_use")){
            const char *id=sget(b,"id"),*name=sget(b,"name");
            if(!*id||!*name||!json_object_is_type(get(b,"input"),json_type_object))goto bad;
            for(size_t k=0;k<alen(calls.get());k++)if(!strcmp(sget(at(calls.get(),k),"id"),id))goto bad;
            J *call=obj(),*f=obj();
            put(call,"id",str(id));
            put(call,"type",str("function"));
            put(f,"name",str(name));
            put(f,"arguments",str(json_object_to_json_string_ext(get(b,"input"),JSON_C_TO_STRING_PLAIN)));
            put(call,"function",f);
            add(calls.get(),call);
        }
        else if(strcmp(type,"thinking")&&strcmp(type,"redacted_thinking"))goto bad;
    }
    if((!strcmp(stop,"tool_use"))!=(alen(calls.get())>0))goto bad;
    put(result.get(),"role",str("assistant"));
    put(result.get(),"content",text.empty()?nullptr:str(text.c_str()));
    put(result.get(),"_anthropic_content",copy(blocks));
    if(alen(calls.get()))put(result.get(),"tool_calls",calls.release());
    return result.release();
    bad:
    seterr(a,tr("Anthropic 工具或内容块格式无效，本轮工具未执行。"));
    return NULL;
}
static std::uint64_t reported_tokens(J *usage,bool native){
    auto count=[usage](const char *name){
        J *value=get(usage,name);
        if(!json_object_is_type(value,json_type_int)||json_object_get_int64(value)<0)return std::uint64_t{0};
        return json_object_get_uint64(value);
    };
    if(native){
        auto input=add_tokens(count("input_tokens"),count("cache_creation_input_tokens"));
        input=add_tokens(input,count("cache_read_input_tokens"));
        return add_tokens(input,count("output_tokens"));
    }
    auto total=count("total_tokens");
    return total?total:add_tokens(count("prompt_tokens"),count("completion_tokens"));
}
J *api_complete(App *a,J *c,J *messages,J *tools){
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    J *body;
    if(native)body=native_payload(c,messages,tools);
    else{
        body=obj();
        put(body,"model",str(sget(c,"model")));
        put(body,"messages",copy(messages));
        if(*sget(c,"reasoning_effort"))put(body,"reasoning_effort",str(sget(c,"reasoning_effort")));
        if(alen(tools)){
            put(body,"tools",copy(tools));
            put(body,"tool_choice",str("auto"));
        }
    }
    StreamState stream;
    stream.native=native;
    // Streamed text is handed to the caller as it arrives; tool arguments and
    // the stop reason are assembled into a normal response below so the same
    // validation runs for both paths.
    stream.delta=[a](const std::string &part){
        if(a->stream_text)a->stream_text(part);
    };
    put(body,"stream",json_object_new_boolean(1));
    J *data=api_request(a,c,native?"/messages":"/chat/completions",body,&stream);
    json_object_put(body);
    if(!data)return NULL;
    if(stream.events){
        if(!native&&stream.stop_reason!="length"&&stream.stop_reason!=(stream.ids.empty()?"stop":"tool_calls")){
            json_object_put(data);
            seterr(a,tr("模型拒绝或尚未结束响应，本轮工具未执行。"));
            return nullptr;
        }
        // Rebuild the response the non-streaming path expects.
        J *usage=json_tokener_parse(stream.usage.c_str());
        put(data,"usage",usage?usage:obj());
        if(native){
            J *blocks=arr();
            if(!stream.text.empty()){J *b=obj();put(b,"type",str("text"));put(b,"text",str(stream.text.c_str()));add(blocks,b);}
            for(size_t i=0;i<stream.ids.size();i++){
                J *b=obj();put(b,"type",str("tool_use"));put(b,"id",str(stream.ids[i].c_str()));put(b,"name",str(stream.names[i].c_str()));
                J *input=stream.args[i].empty()?obj():parse_tool_arguments(stream.args[i].c_str());
                put(b,"input",input);
                add(blocks,b);
            }
            put(data,"content",blocks);
            put(data,"stop_reason",str(stream.stop_reason.c_str()));
        }else{
            J *choices=arr(),*choice=obj(),*message=obj();
            put(message,"role",str("assistant"));
            if(!stream.text.empty())put(message,"content",str(stream.text.c_str()));
            J *calls=arr();
            for(size_t i=0;i<stream.ids.size();i++){
                J *call=obj(),*f=obj();
                put(call,"id",str(stream.ids[i].c_str()));
                put(call,"type",str("function"));
                put(f,"name",str(stream.names[i].c_str()));
                put(f,"arguments",json_object_new_string_len(stream.args[i].data(),static_cast<int>(stream.args[i].size())));
                put(call,"function",f);
                add(calls,call);
            }
            if(alen(calls))put(message,"tool_calls",calls);
            else json_object_put(calls);
            put(choice,"message",message);
            put(choice,"finish_reason",str(stream.stop_reason.c_str()));
            add(choices,choice);
            put(data,"choices",choices);
        }
    }
    a->task_tokens=add_tokens(a->task_tokens,reported_tokens(get(data,"usage"),native));
    task_tokens(*a);
    if(!record_tokens(*a,c,reported_tokens(get(data,"usage"),native)))
        fputs(tr("无法保存词元用量。\n"),stderr);
    J *result=NULL;
    if(native)result=native_response(a,data);
    else{
        J *choice=at(get(data,"choices"),0);
        if(!strcmp(sget(choice,"finish_reason"),"length"))seterr(a,tr("模型响应被截断，本轮工具未执行。"));
        else if(get(get(choice,"message"),"refusal"))seterr(a,tr("模型拒绝处理本次请求。"));
        else if(json_object_is_type(get(choice,"message"),json_type_object)){
            result=copy(get(choice,"message"));
            if(get(result,"role")&&strcmp(sget(result,"role"),"assistant")){
                json_object_put(result);
                result=NULL;
                seterr(a,tr("模型响应的消息角色无效。"));
            }else if(result)put(result,"role",str("assistant"));
        }
        else seterr(a,tr("Chat Completions 响应格式无效。"));
    }
    json_object_put(data);
    return result;
}
J *api_models(App *a,J *c){
    J *data=api_request(a,c,!strcmp(sget(c,"protocol"),"anthropic")?"/models?limit=1000":"/models",NULL);
    if(!data)return NULL;
    J *entries=get(data,"data"),*out=arr();
    if(!json_object_is_type(entries,json_type_array)){
        seterr(a,tr("服务商不支持模型列表；可手动填写模型名称。"));
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
    if(ok)seterr(a,tr("接口响应成功，但未验证工具调用能力；原配置未改动。"));
    return ok;
}
