#include "desk.hpp"
#include "tasks.hpp"
#include <openssl/evp.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cctype>
#include <errno.h>
char *sha256(const void *data,size_t len){
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int n=0;
    if(!EVP_Digest(data,len,hash,&n,EVP_sha256(),NULL))return NULL;
    char *out=static_cast<char *>(malloc(n*2+1));
    if(!out)return NULL;
    for(unsigned int i=0;i<n;i++)sprintf(out+i*2,"%02x",hash[i]);
    return out;
}
int resolve_file(App *a,const char *path,char out[PATH_MAX]){
    fs::path input=fs_path(path),root=fs_path(a->root);
    for(const auto &part:input)if(part==".."){seterr(a,tr("不接受含 .. 的文件路径。"));return -1;}
#ifdef _WIN32
    // Reject alternate streams, device namespaces, aliases and drive-relative paths.
    std::string raw=path;
    if(raw.rfind("\\\\",0)==0||raw.rfind("//",0)==0||(input.has_root_name()&&!input.is_absolute())){seterr(a,tr("文件路径无效。"));return -1;}
    for(const auto &part:input.relative_path()){
        std::string name=path_text(part);if(name.empty()||name==".")continue;
        if(name.find(':')!=std::string::npos||name.back()=='.'||name.back()==' '){seterr(a,tr("文件路径无效。"));return -1;}
        std::string base=name.substr(0,name.find('.'));for(char &c:base)c=static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if(base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"||(base.size()==4&&(base.rfind("COM",0)==0||base.rfind("LPT",0)==0)&&base[3]>='0'&&base[3]<='9')){seterr(a,tr("文件路径无效。"));return -1;}
    }
#endif
    fs::path candidate=input.is_absolute()?input:root/input;
    std::error_code ec;
#ifdef _WIN32
    fs::path current;
    for(const auto &part:candidate){current/=part;if(path_link(current)){seterr(a,tr("文件工具只能访问工作目录。"));return -1;}}
#endif
    fs::path resolved=fs::weakly_canonical(candidate,ec);
    if(ec){seterr(a,tr("无法解析文件路径。"));return -1;}
    if(!path_inside(root,resolved)){seterr(a,tr("文件工具只能访问工作目录。"));return -1;}
    auto config=fs::weakly_canonical(fs_path(a->config_dir),ec);
    if(!ec&&path_inside(config,resolved)){seterr(a,tr("文件工具不访问模型配置与密钥。"));return -1;}
    for(const auto &part:resolved.lexically_relative(root)){
        std::string name=path_text(part);
#ifdef _WIN32
        for(char &c:name)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
#endif
        if(name==".git"||name==".desk-agent"||name==".ssh"||name==".gnupg"||(name.rfind(".env",0)==0&&(name.size()==4||name[4]=='.')&&name!=".env.example")){
            seterr(a,tr("文件工具不访问凭据、Git 内部文件或备份。"));return -1;
        }
    }
    std::string text=path_text(resolved);
    if(text.size()>=PATH_MAX){seterr(a,tr("文件路径过长。"));return -1;}
    memcpy(out,text.c_str(),text.size()+1);return 0;
}
static bool utf8_text(const char *bytes,size_t n){
    return !memchr(bytes,0,n)&&valid_utf8(std::string_view(bytes,n));
}
static J *file_read(App *a,const char *path){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    size_t n=0;
    char *bytes=read_bytes(full,FILE_LIMIT,&n);
    if(!bytes)return err(tr("无法读取文件，文件可能不是普通文件或超过 64 KB。"));
    if(!utf8_text(bytes,n)){
        free(bytes);
        return err(tr("只支持 UTF-8 文本文件。"));
    }
    char *hash=sha256(bytes,n);
    J *result=obj();
    put(result,"content",json_object_new_string_len(bytes,(int)n));
    put(result,"sha256",str(hash));
    free(hash);
    free(bytes);
    return result;
}
static J *file_list(App *a,const char *path){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    std::error_code ec;fs::directory_iterator directory(fs_path(full),ec);
    if(ec)return err(tr("无法读取文件夹。"));
    J *result=obj(),*entries=arr();
    for(auto it=directory;it!=fs::directory_iterator{}&&alen(entries)<200;it.increment(ec)){
        if(ec)break;
        std::string child=path_text(it->path());char resolved[PATH_MAX];
        if(resolve_file(a,child.c_str(),resolved))continue;
        J *e=obj();put(e,"name",str(path_text(it->path().filename()).c_str()));
        put(e,"directory",json_object_new_boolean(it->is_directory(ec)));
        put(e,"symlink",json_object_new_boolean(path_link(it->path())));add(entries,e);
    }
    put(result,"possibly_truncated",json_object_new_boolean(alen(entries)==200));
    put(result,"entries",entries);
    return result;
}
struct Search {
    App *app;
    const char *query;
    J *matches;
    size_t entries=0,files=0,bytes=0,skipped=0;
    double until;
    bool truncated=false;
};
static bool search_limit(Search *s){
    if(interrupted||s->entries>=10000||s->files>=1000||s->bytes>=8*1024*1024||alen(s->matches)>=50||monotime()>s->until)s->truncated=true;
    return s->truncated;
}
static bool search_skip(const char *name){
    return !strcmp(name,"node_modules")||!strcmp(name,"build")||!strcmp(name,"dist")||!strcmp(name,"target")||!strcmp(name,".venv")||!strcmp(name,"venv")||!strcmp(name,"__pycache__")||!strcmp(name,".cache");
}
static void search_walk(Search *s,const char *path,int depth){
    if(search_limit(s))return;
    s->entries++;
    std::error_code ec;fs::path source=fs_path(path);
    if(path_link(source)){s->skipped++;return;}
    auto status=fs::symlink_status(source,ec);if(ec){s->skipped++;return;}
    char full[PATH_MAX];if(resolve_file(s->app,path,full)){s->skipped++;return;}
    if(fs::is_directory(status)){
        if(depth>=16){s->truncated=true;return;}
        fs::directory_iterator directory(fs_path(full),ec);if(ec){s->skipped++;return;}
        for(auto it=directory;it!=fs::directory_iterator{}&&!search_limit(s);it.increment(ec)){
            if(ec){s->skipped++;break;}
            std::string name=path_text(it->path().filename());if(search_skip(name.c_str())){s->skipped++;continue;}
            std::string child=path_text(it->path());search_walk(s,child.c_str(),depth+1);
        }
        return;
    }
    if(!fs::is_regular_file(status)){s->skipped++;return;}
    s->files++;
    size_t n=0;
    char *data=read_bytes(full,FILE_LIMIT,&n);
    if(!data){s->skipped++;return;}
    s->bytes+=n;
    if(!utf8_text(data,n)){s->skipped++;free(data);return;}
    size_t line=1;
    for(char *p=data;*p&&!search_limit(s);line++){
        char *end=strchr(p,'\n');
        if(!end)end=p+strlen(p);
        char *found=strstr(p,s->query);
        if(found&&found<end){
            J *m=obj();
            const char *relative=full+strlen(s->app->root);
            if(*relative=='/')relative++;
            put(m,"path",str(relative));
            put(m,"line",json_object_new_int64((int64_t)line));
            char *start=p;
            if(found-p>120){
                start=found-120;
                while(((unsigned char)*start&0xc0)==0x80)start++;
            }
            size_t length=(size_t)(end-start);
            if(length>360){
                length=360;
                while(length&&((unsigned char)start[length]&0xc0)==0x80)length--;
            }
            put(m,"text",json_object_new_string_len(start,(int)length));
            put(m,"line_truncated",json_object_new_boolean(start!=p||start+length<end));
            add(s->matches,m);
        }
        if(!*end)break;
        p=end+1;
    }
    free(data);
}
static J *file_search(App *a,J *args){
    const char *query=sget(args,"query");
    if(!*query||strlen(query)>256||strchr(query,'\n')||strchr(query,'\r'))return err(tr("搜索内容须为 1..256 字节的单行文本，按字面匹配。"));
    char full[PATH_MAX];
    if(resolve_file(a,sget(args,"path"),full))return err(a->error);
    std::error_code ec;auto status=fs::status(fs_path(full),ec);
    if(ec||(!fs::is_directory(status)&&!fs::is_regular_file(status)))return err(tr("搜索路径须为存在的文件或目录。"));
    Search s={.app=a,.query=query,.matches=arr(),.until=monotime()+3};
    search_walk(&s,full,0);
    if(interrupted){json_object_put(s.matches);return err(tr("搜索已取消。"));}
    J *r=obj();
    put(r,"matches",s.matches);
    put(r,"files_scanned",json_object_new_int64((int64_t)s.files));
    put(r,"skipped",json_object_new_int64((int64_t)s.skipped));
    put(r,"truncated",json_object_new_boolean(s.truncated));
    if(s.truncated)put(r,"hint",str(tr("达到搜索预算；缩小 path 后继续，结果并非完整列表。")));
    return r;
}
static J *file_write(App *a,J *args,const char *preview){
    const char *path=sget(args,"path"),*content=sget(args,"content");
    size_t n=strlen(content);
    if(n>FILE_LIMIT)return err(tr("文件超过 64 KB 限制。"));
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    std::error_code ec;auto status=fs::symlink_status(fs_path(full),ec);
    bool exists=fs::exists(status);
    if(ec&&ec!=std::errc::no_such_file_or_directory)return err(tr("无法访问目标文件。"));
    if(exists&&!fs::is_regular_file(status))return err(tr("目标不是普通文件。"));
    unsigned int mode=exists?static_cast<unsigned int>(status.permissions())&0777:0644;
    size_t old_len=0;
    char *old=exists?read_bytes(full,FILE_LIMIT,&old_len):NULL;
    if(exists&&!old)return err(tr("无法读取原文件或原文件过大。"));
    char *hash=exists?sha256(old,old_len):NULL;
    J *expected=get(args,"expected_sha256");
    bool matches=exists?(expected&&json_object_is_type(expected,json_type_string)&&hash&&!strcmp(hash,json_object_get_string(expected))):!expected;
    free(hash);
    if(!matches){
        free(old);
        return err(tr("文件已变化，或覆盖前未读取，请重新读取文件。"));
    }
    char *description=NULL;
    if(asprintf(&description,tr("%s %s（%zu 字节）\n%s"),preview?tr("修改"):tr("写入"),path,n,preview?preview:content)<0){
        free(old);
        return err(tr("无法分配写入预览内存，原文件未修改。"));
    }
    int allowed=approve(a,description);
    free(description);
    if(!allowed){
        free(old);
        J *r=err(tr("用户拒绝写入，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    size_t latest_len=0;
    char *latest=read_bytes(full,FILE_LIMIT,&latest_len);
    bool unchanged=exists?(latest&&latest_len==old_len&&!memcmp(old,latest,old_len)):(!latest&&errno==ENOENT);
    free(latest);
    if(!unchanged){
        free(old);
        return err(tr("确认期间文件已变化，未写入。"));
    }
    char backup[PATH_MAX]="";
    if(exists){
        char directory[PATH_MAX],id[33];
        id_string(id);
        if(path_suffix(directory,a->root,"/.desk-agent/backups")||mkdirs(directory,0700)||snprintf(backup,sizeof backup,"%s/%s.bak",directory,id)>=(int)sizeof backup||atomic_file(backup,old,old_len,0600)){
            free(old);
            return err(tr("创建备份失败，未修改原文件。"));
        }
    }
    free(old);
    char parent[PATH_MAX];
    strcpy(parent,full);
    char *slash=strrchr(parent,'/');
    if(slash&&slash!=parent)*slash=0;
    if(mkdirs(parent,0755)||atomic_file(full,content,n,mode))return err(tr("写入文件失败。"));
    latest=read_bytes(full,FILE_LIMIT,&latest_len);
    bool verified=latest&&latest_len==n&&!memcmp(latest,content,n);
    free(latest);
    if(!verified)return err(tr("已写入，但回读核对不一致；请重新读取当前文件，不要直接重试覆盖。"));
    J *result=obj();
    put(result,"written",str(path));
    hash=sha256(content,n);
    put(result,"sha256",str(hash));
    free(hash);
    put(result,"backup",*backup?str(backup):NULL);
    put(result,"verified",json_object_new_boolean(1));
    return result;
}
static J *file_edit(App *a,J *args){
    const char *before=sget(args,"old_text"),*after=sget(args,"new_text");
    if(!*before||!strcmp(before,after))return err(tr("old_text 不能为空，且新旧文本须不同。"));
    JsonPtr original(file_read(a,sget(args,"path")));
    J *read=original.get();
    if(get(read,"error"))return original.release();
    if(strcmp(sget(read,"sha256"),sget(args,"expected_sha256"))){
        return err(tr("文件已变化，或修改前未读取，请重新读取文件。"));
    }
    const char *content=sget(read,"content"),*match=strstr(content,before);
    if(!match||strstr(match+1,before)){
        return err(tr("old_text 必须在文件中精确匹配一次；请加入相邻上下文后重试。"));
    }
    size_t size=strlen(content)-strlen(before)+strlen(after),offset=(size_t)(match-content);
    if(size>FILE_LIMIT)return err(tr("修改后的文件超过 64 KB 限制。"));
    std::string updated(content);
    updated.replace(offset,strlen(before),after);
    const std::string preview=std::string(tr("替换前：\n"))+before+tr("\n替换后：\n")+after;
    JsonPtr write(copy(args));
    json_object_object_del(write.get(),"old_text");
    json_object_object_del(write.get(),"new_text");
    put(write.get(),"content",str(updated.c_str()));
    return file_write(a,write.get(),preview.c_str());
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
        if(!strcmp(fields[i],"expected_sha256")&&!strcmp(name,"write_file")){
            J *types=arr();
            add(types,str("string"));
            add(types,str("null"));
            put(value,"type",types);
        }
        else put(value,"type",str("string"));
        const char *description=!strcmp(fields[i],"path")?"Workspace-relative file or directory path; use . for the workspace root.":
            !strcmp(fields[i],"expected_sha256")?"SHA256 from the latest read_file result; null only when creating a new file.":
            !strcmp(fields[i],"old_text")?"Exact original text, including whitespace. Must occur exactly once; include surrounding lines to disambiguate.":
            !strcmp(fields[i],"new_text")?"Replacement text; empty string deletes the matched text.":
            !strcmp(fields[i],"query")?"Case-sensitive literal single-line search text, not a regular expression (1..256 bytes).":
            !strcmp(fields[i],"content")?"Complete UTF-8 file contents (up to 64 KB).":
            !strcmp(fields[i],"command")?"One noninteractive bash command. Runs independently in the workspace; cd and exported variables do not persist.":
            !strcmp(fields[i],"app")?"Empty string lists applications; otherwise use an exact application name from the latest snapshot.":
            !strcmp(fields[i],"ref")?"Fresh control reference from the latest snapshot; invalid after any desktop action.":
            !strcmp(fields[i],"action")?"Exact action name advertised by the referenced control.":"Complete replacement text for the editable control.";
        put(value,"description",str(description));
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
    ,*search[]={
        "path","query"
    }
    ,*edit[]={
        "path","old_text","new_text","expected_sha256"
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
    definition(tools,"list_files","List up to 200 immediate workspace entries. Explore relevant directories before guessing file names.",path,1);
    definition(tools,"search_text","Recursively locate literal text with file paths and line numbers. Skips symlinks, protected files, common dependency/build directories and non-UTF-8 or >64 KB files. Up to 50 matching lines, 1000 files, depth 16, 8 MB, 3 seconds; inspect truncated and skipped before claiming absence. Narrow path to continue.",search,2);
    definition(tools,"read_file","Read a complete UTF-8 file (max 64 KB) and its SHA256. Required before editing an existing file; search snippets are not sufficient.",path,1);
    definition(tools,"edit_file","Replace one unique exact text occurrence in an existing file. Prefer this for small edits. Supply SHA256 from read_file; use longer old_text if ambiguous. Preserves other bytes and creates a backup. Result verified means bytes were read back, not that the user's goal or tests passed.",edit,4);
    definition(tools,"write_file","Create a UTF-8 file or replace its entire contents. Prefer edit_file for small changes. Read existing file first and supply SHA256; null only for a new file. Original bytes are backed up and new bytes read back. Verify the task's actual behavior separately.",write,3);
    if(a->shell)definition(tools,"run_shell",
#ifdef _WIN32
        "Execute noninteractive Windows PowerShell in the workspace, 30s timeout. Use PowerShell syntax, not bash. There is no sandbox. Calls do not share variables or working-directory changes. Do not start persistent/background services. Inspect exit_code, timed_out and output to verify the result.",
#else
        "Execute noninteractive bash in workspace, 30s timeout, no sandbox. Do not start persistent/background services. Separate calls do not preserve cd or variables. Inspect exit_code, timed_out and output; on failure diagnose and change approach rather than repeating. A successful command alone does not prove task completion.",
#endif
        cmd,1);
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
        return err(tr("未知或未启用的工具，或参数不是对象。"));
    }
    J *properties=get(spec,"properties");
    bool valid=json_object_object_length(args)==json_object_object_length(properties);
    json_object_object_foreach(properties,key,description){
        J *value=NULL;
        bool nullable=json_object_is_type(get(description,"type"),json_type_array);
        if(!json_object_object_get_ex(args,key,&value)||(!json_object_is_type(value,json_type_string)&&!(value==NULL&&nullable)))valid=false;
        if(json_object_is_type(value,json_type_string)&&(size_t)json_object_get_string_len(value)!=strlen(json_object_get_string(value)))valid=false;
    }
    json_object_put(defs);
    if(!valid)return err(tr("工具参数缺失、多余或类型不正确。"));
    std::unique_ptr<ResourceLock> lock;
    if(!strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"run_shell")){
        lock=std::make_unique<ResourceLock>(*a,false);
        if(!lock->acquired()||interrupted)return err(interrupted?tr("资源等待已取消。"):tr("工作目录被占用或无法加锁，未执行操作；请进入任务管理处理待确认操作，或新建并行任务。"));
    }
    if(!strcmp(name,"list_files"))return file_list(a,sget(args,"path"));
    if(!strcmp(name,"read_file"))return file_read(a,sget(args,"path"));
    if(!strcmp(name,"search_text"))return file_search(a,args);
    if(!strcmp(name,"edit_file"))return file_edit(a,args);
    if(!strcmp(name,"write_file"))return file_write(a,args,NULL);
    if(!strcmp(name,"run_shell")){
        if(!approve(a,sget(args,"command"))){
            J *r=err(tr("用户拒绝执行，不要绕过。"));
            put(r,"denied",json_object_new_boolean(1));
            return r;
        }
        return run_command(a,sget(args,"command"),30000);
    }
    if(!strcmp(name,"desktop_snapshot"))return desktop_snapshot(a,sget(args,"app"));
    if(!approve(a,json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN))){
        J *r=err(tr("用户拒绝桌面操作，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    return desktop_act(a,sget(args,"ref"),!strcmp(name,"desktop_action")?sget(args,"action"):NULL,!strcmp(name,"desktop_set_text")?sget(args,"text"):NULL);
}
