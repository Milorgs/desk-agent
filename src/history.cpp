#include "desk.hpp"
#include "tasks.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
bool valid_id(std::string_view id){
    return id.size()==32&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::string path(const App &app,std::string_view id,const char *suffix){
    return std::string(app.config_dir)+"/history/"+std::string(id)+suffix;
}
JsonPtr read(const std::string &file,size_t limit=32000000){
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(file.c_str(),limit,&length),free);
    if(!bytes)return {};
    JsonPtr document(json_tokener_parse(bytes.get()));
    if(!document||!json_object_is_type(document.get(),json_type_object))return {};
    return document;
}
bool save(const App &app,const std::string &file,J *document){
    auto directory=std::string(app.config_dir)+"/history";
    if(mkdirs(directory.c_str(),0700))return false;
    const char *data=json_object_to_json_string_ext(document,JSON_C_TO_STRING_PLAIN);
    return !atomic_file(file.c_str(),data,strlen(data),0600);
}
bool matching(const App &app,J *record){
    return !strcmp(sget(record,"profile"),sget(app.config,"profile"))
        &&!strcmp(sget(record,"model"),sget(app.config,"model"))
        &&!strcmp(sget(record,"protocol"),sget(app.config,"protocol"));
}
}
J *history_read(const App &app,const std::string &id){
    if(!valid_id(id))return nullptr;
    auto document=read(path(app,id,".data.json"));
    return document.release();
}
J *history_context(const App &app,const std::string &id){
    JsonPtr document(history_read(app,id));
    J *context=get(document.get(),"context");
    return matching(app,document.get())&&json_object_is_type(context,json_type_array)?copy(context):arr();
}
J *history_list(const App &app){
    JsonPtr entries(arr());
    std::error_code error;
    fs::directory_iterator iterator(fs_path(std::string(app.config_dir)+"/history"),error);
    if(error)return entries.release();
    std::vector<JsonPtr> records;
    for(fs::directory_iterator end;iterator!=end;iterator.increment(error)){
        if(error)break;
        auto name=path_text(iterator->path().filename());
        if(name.size()!=42||!name.ends_with(".meta.json"))continue;
        auto id=name.substr(0,32);
        if(!valid_id(id))continue;
        auto record=read(path_text(iterator->path()),65536);
        if(!record||bget(record.get(),"deleted")||strcmp(sget(record.get(),"workspace"),app.root))continue;
        put(record.get(),"id",str(id.c_str()));
        records.push_back(std::move(record));
    }
    std::sort(records.begin(),records.end(),[](const auto &left,const auto &right){
        return json_object_get_double(get(left.get(),"updated"))>json_object_get_double(get(right.get(),"updated"));
    });
    for(auto &record:records)add(entries.get(),record.release());
    return entries.release();
}
bool history_save(const App &app,const std::string &id,const std::string &title,J *config,bool deleted){
    if(!valid_id(id))return false;
    if(mkdirs((std::string(app.config_dir)+"/history").c_str(),0700))return false;
    ResourceLock lock(app.config_dir);
    if(!lock.acquired())return false;
    JsonPtr metadata(obj());
    put(metadata.get(),"workspace",str(app.root));
    put(metadata.get(),"title",str(title.c_str()));
    put(metadata.get(),"profile",str(sget(config,"profile")));
    put(metadata.get(),"model",str(sget(config,"model")));
    put(metadata.get(),"deleted",json_object_new_boolean(deleted));
    if(deleted)for(auto key:{"title","profile","model"})json_object_object_del(metadata.get(),key);
    double now=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    put(metadata.get(),"updated",json_object_new_double(now));
    if(!save(app,path(app,id,".meta.json"),metadata.get()))return false;
    if(deleted){std::error_code error;fs::remove(fs_path(path(app,id,".data.json")),error);return !error;}
    return true;
}
void history_checkpoint(App &app,size_t first){
    if(!valid_id(app.conversation_id))return;
    ResourceLock lock(app.config_dir);
    if(!lock.acquired())return;
    JsonPtr metadata=read(path(app,app.conversation_id,".meta.json"),65536);
    if(!metadata||bget(metadata.get(),"deleted"))return;
    JsonPtr document(history_read(app,app.conversation_id));
    if(!document){
        std::error_code error;
        if(fs::exists(fs_path(path(app,app.conversation_id,".data.json")),error)||error){
            fputs(tr("对话记录无法读取，已保留原文件。\n"),stderr);
            return;
        }
        document.reset(obj());
    }
    J *transcript=get(document.get(),"transcript");
    if(!json_object_is_type(transcript,json_type_array)){transcript=arr();put(document.get(),"transcript",transcript);}
    for(size_t i=first;i<alen(app.messages);i++)add(transcript,copy(at(app.messages,i)));
    put(document.get(),"context",copy(app.messages));
    for(auto key:{"profile","model","protocol"})put(document.get(),key,str(sget(app.config,key)));
    if(!save(app,path(app,app.conversation_id,".data.json"),document.get()))
        fputs(tr("无法保存对话记录，请检查本机配置目录。\n"),stderr);
}
