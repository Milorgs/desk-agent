#include "desk.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static bool inside(const char *root,const char *path){
    size_t n=strlen(root);
    return !strncmp(root,path,n)&&(path[n]=='/'||!path[n]||!strcmp(root,"/"));
}
char *sha256(const void *data,size_t len){
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int n=0;
    if(!EVP_Digest(data,len,hash,&n,EVP_sha256(),NULL))return NULL;
    char *out=malloc(n*2+1);
    if(!out)return NULL;
    for(unsigned int i=0;i<n;i++)sprintf(out+i*2,"%02x",hash[i]);
    return out;
}
int resolve_file(App *a,const char *path,char out[PATH_MAX]){
    char raw[PATH_MAX];
    int length=path[0]=='/'?snprintf(raw,sizeof raw,"%s",path):snprintf(raw,sizeof raw,"%s/%s",a->root,path);
    if(length>=(int)sizeof raw){
        seterr(a,"文件路径过长。");
        return -1;
    }
    char check[PATH_MAX];
    strcpy(check,raw);
    char *save=NULL;
    for(char *p=strtok_r(check,"/",&save);p;p=strtok_r(NULL,"/",&save))if(!strcmp(p,"..")){
        seterr(a,"不接受含 .. 的文件路径。");
        return -1;
    }
    if(!realpath(raw,out)){
        if(errno!=ENOENT){
            seterr(a,"无法解析文件路径。");
            return -1;
        }
        char prefix[PATH_MAX],tail[PATH_MAX]="";
        strcpy(prefix,raw);
        while(!realpath(prefix,out)){
            char *slash=strrchr(prefix,'/');
            if(!slash){
                seterr(a,"文件路径无效。");
                return -1;
            }
            char next[PATH_MAX];
            if(snprintf(next,sizeof next,"/%s%s",slash+1,tail)>=(int)sizeof next)return -1;
            strcpy(tail,next);
            if(slash==prefix){
                prefix[1]=0;
            }
            else *slash=0;
        }
        if(strlen(out)+strlen(tail)>=PATH_MAX)return -1;
        strcat(out,tail);
    }
    if(!inside(a->root,out)){
        seterr(a,"文件工具只能访问工作目录。");
        return -1;
    }
    char cfg[PATH_MAX];
    if(realpath(a->config_dir,cfg)&&inside(cfg,out)){
        seterr(a,"文件工具不访问模型配置与密钥。");
        return -1;
    }
    const char *relative=out+strlen(a->root);
    strncpy(check,relative,sizeof check-1);
    check[sizeof check-1]=0;
    save=NULL;
    for(char *p=strtok_r(check,"/",&save);p;p=strtok_r(NULL,"/",&save))if(!strcmp(p,".git")||!strcmp(p,".desk-agent")||!strcmp(p,".ssh")||!strcmp(p,".gnupg")||(!strncmp(p,".env",4)&&(!p[4]||p[4]=='.')&&strcmp(p,".env.example"))){
        seterr(a,"文件工具不访问凭据、Git 内部文件或备份。");
        return -1;
    }
    return 0;
}
static J *file_read(App *a,const char *path){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    size_t n=0;
    char *bytes=read_bytes(full,FILE_LIMIT,&n);
    if(!bytes)return err("无法读取文件，文件可能不是普通文件或超过 64 KB。");
    struct json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_VALIDATE_UTF8);
    J *quoted=str("");
    json_object_put(quoted);
    /* json-c string constructors accept bytes, validate using the serialized string. */ J *value=json_object_new_string_len(bytes,(int)n);
    const char *serialized=json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
    J *parsed=json_tokener_parse_ex(tok,serialized,(int)strlen(serialized));
    bool valid=json_tokener_get_error(tok)==json_tokener_success;
    json_object_put(parsed);
    json_tokener_free(tok);
    if(!valid||memchr(bytes,0,n)){
        json_object_put(value);
        free(bytes);
        return err("只支持 UTF-8 文本文件。");
    }
    char *hash=sha256(bytes,n);
    J *result=obj();
    put(result,"content",value);
    put(result,"sha256",str(hash));
    free(hash);
    free(bytes);
    return result;
}
static J *file_list(App *a,const char *path){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    DIR *directory=opendir(full);
    if(!directory)return err("无法读取文件夹。");
    J *result=obj(),*entries=arr();
    struct dirent *entry;
    while((entry=readdir(directory))&&alen(entries)<200){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char child[PATH_MAX],resolved[PATH_MAX];
        if(snprintf(child,sizeof child,"%s/%s",full,entry->d_name)>=(int)sizeof child||resolve_file(a,child,resolved))continue;
        struct stat st;
        if(lstat(child,&st))continue;
        J *e=obj();
        put(e,"name",str(entry->d_name));
        put(e,"directory",json_object_new_boolean(S_ISDIR(st.st_mode)));
        put(e,"symlink",json_object_new_boolean(S_ISLNK(st.st_mode)));
        add(entries,e);
    }
    closedir(directory);
    put(result,"possibly_truncated",json_object_new_boolean(alen(entries)==200));
    put(result,"entries",entries);
    return result;
}
static J *file_write(App *a,J *args){
    const char *path=sget(args,"path"),*content=sget(args,"content");
    size_t n=strlen(content);
    if(n>FILE_LIMIT)return err("文件超过 64 KB 限制。");
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    struct stat st;
    bool exists=lstat(full,&st)==0;
    if(!exists&&errno!=ENOENT)return err("无法访问目标文件。");
    if(exists&&!S_ISREG(st.st_mode))return err("目标不是普通文件。");
    size_t old_len=0;
    char *old=exists?read_bytes(full,FILE_LIMIT,&old_len):NULL;
    if(exists&&!old)return err("无法读取原文件或原文件过大。");
    char *hash=exists?sha256(old,old_len):NULL;
    J *expected=get(args,"expected_sha256");
    bool matches=exists?(expected&&json_object_is_type(expected,json_type_string)&&hash&&!strcmp(hash,json_object_get_string(expected))):!expected;
    free(hash);
    if(!matches){
        free(old);
        return err("文件已变化，或覆盖前未读取，请重新读取文件。");
    }
    char *description=NULL;
    asprintf(&description,"写入 %s（%zu 字节）\n%s",path,n,content);
    int allowed=approve(a,description);
    free(description);
    if(!allowed){
        free(old);
        J *r=err("用户拒绝写入，不要绕过。");
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    size_t latest_len=0;
    char *latest=read_bytes(full,FILE_LIMIT,&latest_len);
    bool unchanged=exists?(latest&&latest_len==old_len&&!memcmp(old,latest,old_len)):(!latest&&errno==ENOENT);
    free(latest);
    if(!unchanged){
        free(old);
        return err("确认期间文件已变化，未写入。");
    }
    char backup[PATH_MAX]="";
    if(exists){
        char directory[PATH_MAX],id[33];
        id_string(id);
        if(path_suffix(directory,a->root,"/.desk-agent/backups")||mkdirs(directory,0700)||snprintf(backup,sizeof backup,"%s/%s.bak",directory,id)>=(int)sizeof backup||atomic_file(backup,old,old_len,0600)){
            free(old);
            return err("创建备份失败，未修改原文件。");
        }
    }
    free(old);
    char parent[PATH_MAX];
    strcpy(parent,full);
    char *slash=strrchr(parent,'/');
    if(slash&&slash!=parent)*slash=0;
    if(mkdirs(parent,0755)||atomic_file(full,content,n,exists?(st.st_mode&0777):0644))return err("写入文件失败。");
    J *result=obj();
    put(result,"written",str(path));
    hash=sha256(content,n);
    put(result,"sha256",str(hash));
    free(hash);
    put(result,"backup",*backup?str(backup):NULL);
    return result;
}
static void definition(J *tools,const char *name,const char *description,const char **fields,int count){
    J *t=obj(),*f=obj(),*p=obj(),*properties=obj(),*required=arr();
    put(t,"type",str("function"));
    put(f,"name",str(name));
    put(f,"description",str(description));
    put(p,"type",str("object"));
    put(p,"properties",properties);
    put(p,"required",required);
    put(p,"additionalProperties",json_object_new_boolean(0));
    for(int i=0;i<count;i++){
        J *value=obj();
        if(!strcmp(fields[i],"expected_sha256")){
            J *types=arr();
            add(types,str("string"));
            add(types,str("null"));
            put(value,"type",types);
        }
        else put(value,"type",str("string"));
        put(properties,fields[i],value);
        add(required,str(fields[i]));
    }
    put(f,"parameters",p);
    put(t,"function",f);
    add(tools,t);
}
J *schemas(App *a){
    J *tools=arr();
    const char *path[]={
        "path"
    }
    ,*write[]={
        "path","content","expected_sha256"
    }
    ,*cmd[]={
        "command"
    }
    ,*app[]={
        "app"
    }
    ,*act[]={
        "ref","action"
    }
    ,*text[]={
        "ref","text"
    };
    definition(tools,"list_files","List up to 200 workspace entries.",path,1);
    definition(tools,"read_file","Read UTF-8 file, max 64 KB, and SHA256.",path,1);
    definition(tools,"write_file","Write UTF-8 file. Read existing file first and supply SHA256. Use null for new file. Original bytes are backed up.",write,3);
    if(a->shell)definition(tools,"run_shell","Execute a shell command in workspace, 30s timeout, no sandbox. No persistent background services.",cmd,1);
    if(a->desktop){
        definition(tools,"desktop_snapshot","Read apps with empty app, then exact app name for controls. Text is sent to model API.",app,1);
        definition(tools,"desktop_action","Invoke an action advertised by current snapshot. Re-snapshot after action.",act,2);
        definition(tools,"desktop_set_text","Replace entire editable control text. Re-snapshot to verify.",text,2);
    }
    return tools;
}
J *tool_call(App *a,const char *name,J *args){
    J *defs=schemas(a),*spec=NULL;
    for(size_t i=0;i<alen(defs);i++){
        J *f=get(at(defs,i),"function");
        if(!strcmp(sget(f,"name"),name)){
            spec=get(f,"parameters");
            break;
        }
    }
    if(!spec||!json_object_is_type(args,json_type_object)){
        json_object_put(defs);
        return err("未知或未启用的工具，或参数不是对象。");
    }
    J *properties=get(spec,"properties");
    bool valid=json_object_object_length(args)==json_object_object_length(properties);
    json_object_object_foreach(properties,key,description){
        (void)description;
        J *value=NULL;
        if(!json_object_object_get_ex(args,key,&value)||(!json_object_is_type(value,json_type_string)&&!(value==NULL&&!strcmp(key,"expected_sha256"))))valid=false;
        if(json_object_is_type(value,json_type_string)&&(size_t)json_object_get_string_len(value)!=strlen(json_object_get_string(value)))valid=false;
    }
    json_object_put(defs);
    if(!valid)return err("工具参数缺失、多余或类型不正确。");
    if(!strcmp(name,"list_files"))return file_list(a,sget(args,"path"));
    if(!strcmp(name,"read_file"))return file_read(a,sget(args,"path"));
    if(!strcmp(name,"write_file"))return file_write(a,args);
    if(!strcmp(name,"run_shell")){
        if(!approve(a,sget(args,"command"))){
            J *r=err("用户拒绝执行，不要绕过。");
            put(r,"denied",json_object_new_boolean(1));
            return r;
        }
        return run_command(a,sget(args,"command"),30000);
    }
    if(!strcmp(name,"desktop_snapshot"))return desktop_snapshot(a,sget(args,"app"));
    if(!approve(a,json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN))){
        J *r=err("用户拒绝桌面操作，不要绕过。");
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    return desktop_act(a,sget(args,"ref"),!strcmp(name,"desktop_action")?sget(args,"action"):NULL,!strcmp(name,"desktop_set_text")?sget(args,"text"):NULL);
}
