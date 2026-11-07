#include <sys/ioctl.h>
#include "tasks.hpp"
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <pty.h>
#include <poll.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace {
bool write_all(int fd,std::string_view text){
    while(!text.empty()){
        ssize_t n=write(fd,text.data(),text.size());
        if(n<0&&errno==EINTR&&!interrupted)continue;
        if(n<=0)return false;
        text.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}
void nonblocking(int fd){fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);}
struct Job {
    int id=0,master=-1,events=-1,feed=-1;
    pid_t pid=-1;
    bool active=true,alive=true,cancelled=false,truncated=false;
    bool retired=false;
    double retired_at=0;
    bool thinking=false;
    double thinking_started=0;
    double changed_at=0;
    // Wall time of the current turn and tokens the worker has reported for it.
    double turn_started=0;
    std::uint64_t tokens=0;
    std::string pending_events;
    std::string title,workspace,state="执行中",output;
};
void drain(Job &j){
    char buffer[4096];
    for(;;){
        ssize_t n=read(j.master,buffer,sizeof buffer);
        if(n<=0)break;
        j.output.append(buffer,static_cast<size_t>(n));
        if(j.output.size()>65536){
            size_t cut=j.output.size()-65536;
            while(cut<j.output.size()&&(static_cast<unsigned char>(j.output[cut])&0xc0)==0x80)cut++;
            j.output.erase(0,cut);j.truncated=true;
        }
    }
}
}
void task_event(App &app,char state){
    if(app.task_events>=0){ssize_t n=write(app.task_events,&state,1);(void)n;}
}
void task_tokens(App &app){
    if(app.task_events<0)return;
    char message[9];message[0]='k';
    std::uint64_t value=app.task_tokens;memcpy(message+1,&value,sizeof value);
    ssize_t n=write(app.task_events,message,sizeof message);(void)n;
}
ResourceLock::ResourceLock(App &app,bool desktop){
    std::string directory=desktop?app.config_dir:std::string(app.root)+"/.desk-agent";
    if(mkdirs(directory.c_str(),0700))return;
    std::string path=directory+(desktop?"/desktop.lock":"/operations.lock");
    int candidate=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(candidate<0)return;
    bool waiting=false;
    while(!interrupted){
        if(!flock(candidate,LOCK_EX|LOCK_NB)){
            fd=candidate;
            if(waiting)task_event(app,'r');
            return;
        }
        if(errno!=EWOULDBLOCK&&errno!=EAGAIN)break;
        // Do not block the control terminal behind a task awaiting approval.
        if(app.task_events<0){puts(tr("资源正被任务占用，请返回对应对话处理待批准操作。"));break;}
        if(!waiting){puts(desktop?tr("等待其他任务释放桌面…"):tr("等待同一工作目录的其他操作结束…"));fflush(stdout);task_event(app,'q');waiting=true;}
        usleep(50000);
    }
    close(candidate);
}
ResourceLock::ResourceLock(const char *config_dir){
    auto path=std::string(config_dir)+"/token-usage.lock";
    int handle=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(handle<0)return;
    int result;
    do{result=flock(handle,LOCK_EX);}while(result<0&&errno==EINTR);
    if(result==0)fd=handle;
    else close(handle);
}
ResourceLock::~ResourceLock(){if(fd>=0){flock(fd,LOCK_UN);close(fd);}}
struct Tasks::Impl {
    std::vector<Job> jobs;
    Impl(){jobs.reserve(32);}
    int next=1;
    std::string executable;
    Job *find(int id){for(auto &j:jobs)if(j.id==id&&!j.retired)return &j;return nullptr;}
};
Tasks::Tasks(std::string executable):impl(std::make_unique<Impl>()){impl->executable=std::move(executable);}
Tasks::~Tasks(){
    for(auto &j:impl->jobs)if(j.alive){j.cancelled=true;kill(j.pid,SIGTERM);}
    double until=monotime()+10;
    while(monotime()<until){refresh();if(std::none_of(impl->jobs.begin(),impl->jobs.end(),[](const Job &j){return j.alive;}))break;usleep(20000);}
    for(auto &j:impl->jobs){
        if(j.alive){kill(j.pid,SIGKILL);while(waitpid(j.pid,nullptr,0)<0&&errno==EINTR){}}
        close(j.master);close(j.events);close(j.feed);
    }
}
void Tasks::refresh(){
    for(auto &j:impl->jobs){
        char events[128];ssize_t n;
        while((n=read(j.events,events,sizeof events))>0)j.pending_events.append(events,static_cast<size_t>(n));
        size_t used=0;
        for(size_t k=0;k<j.pending_events.size();k++){
            char event=j.pending_events[k];
            if(event=='k'){
                // A token update carries an 8-byte count; wait for all of it.
                if(k+9>j.pending_events.size())break;
                std::uint64_t value=0;memcpy(&value,j.pending_events.data()+k+1,sizeof value);
                j.tokens=value;k+=8;used=k+1;continue;
            }
            used=k+1;
            j.changed_at=monotime();
            if(event=='t'){j.thinking=true;j.thinking_started=monotime();continue;}
            if(event=='e'){j.thinking=false;continue;}
            j.thinking=false;
            if(event=='d'){j.state="已完成";j.active=false;}
            else if(event=='f'){j.state="失败";j.active=false;}
            else if(event=='c'){j.state="已取消";j.active=false;}
            else if(event=='a')j.state="等待确认";
            else if(event=='q')j.state="等待资源";
            else if(event=='r')j.state="执行中";
        }
        j.pending_events.erase(0,used);
        drain(j);
        if(!j.alive)continue;
        if(j.retired&&monotime()-j.retired_at>10)kill(j.pid,SIGKILL);
        int status=0;
        pid_t ended=waitpid(j.pid,&status,WNOHANG);
        if(ended==j.pid){
            j.changed_at=monotime();
            j.alive=false;
            if(j.active)j.state=j.cancelled||(WIFEXITED(status)&&WEXITSTATUS(status)==2)?"已取消":WIFEXITED(status)&&WEXITSTATUS(status)==0?"已完成":"失败";
            j.active=false;
            drain(j);
        }
    }
    std::erase_if(impl->jobs,[](Job &j){
        if(!j.retired||j.alive)return false;
        close(j.master);close(j.events);if(j.feed>=0)close(j.feed);
        return true;
    });
}
int Tasks::start(App &app,const std::string &task,const std::string &workspace){
    refresh();
    if(task.empty()||task.size()>65536||std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const Job &j){return !j.retired;})>=32||std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const Job &j){return j.active;})>=4){
        seterr(&app,tr("任务为空、过长或达到上限（同时 4 个，最多保留 32 个）。"));return -1;
    }
    if(config_valid(&app,app.config))return -1;
    char root[PATH_MAX];
    struct stat directory{};
    if(!realpath(workspace.empty()?app.root:workspace.c_str(),root)||stat(root,&directory)||!S_ISDIR(directory.st_mode)){seterr(&app,tr("工作目录不存在。"));return -1;}
    int payload[2],events[2];
    if(pipe2(payload,O_CLOEXEC)){seterr(&app,tr("无法创建任务管道。"));return -1;}
    if(pipe2(events,O_CLOEXEC|O_NONBLOCK)){close(payload[0]);close(payload[1]);seterr(&app,tr("无法创建状态管道。"));return -1;}
    JsonPtr data(obj());
    put(data.get(),"conversation_id",str(app.conversation_id.c_str()));
    put(data.get(),"language",str(language_code()));put(data.get(),"config",copy(app.config));put(data.get(),"root",str(root));put(data.get(),"config_dir",str(app.config_dir));
    put(data.get(),"conversation",json_object_new_boolean(app.conversation_worker));put(data.get(),"task",str(task.c_str()));put(data.get(),"yes",json_object_new_boolean(app.yes));put(data.get(),"auto_edit",json_object_new_boolean(app.auto_edit));
    put(data.get(),"guided_approval",json_object_new_boolean(app.guided_approval));
    put(data.get(),"shell",json_object_new_boolean(app.shell));put(data.get(),"desktop",json_object_new_boolean(app.desktop));
    put(data.get(),"record",json_object_new_boolean(app.record));put(data.get(),"verbose",json_object_new_boolean(app.verbose));put(data.get(),"max_steps",json_object_new_int(app.max_steps));
    put(data.get(),"events",json_object_new_int(events[1]));
    std::string body=json_object_to_json_string_ext(data.get(),JSON_C_TO_STRING_PLAIN);
    if(body.size()>250000){close(payload[0]);close(payload[1]);close(events[0]);close(events[1]);seterr(&app,tr("任务配置过大。"));return -1;}
    // Flush inherited stdio; a worker execs a fresh process, including AT-SPI.
    fflush(nullptr);
    int master=-1;
    pid_t controller=getpid();
    // Give the worker the controller's size so panels and rules it prints fit
    // the real window instead of falling back to the borderless layout.
    struct winsize size{};
    const bool sized=ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0&&size.ws_col>0;
    pid_t pid=forkpty(&master,nullptr,nullptr,sized?&size:nullptr);
    if(pid==0){
        if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=controller)_exit(127);
        close(payload[1]);close(events[0]);
        fcntl(payload[0],F_SETFD,0);fcntl(events[1],F_SETFD,0);
        char number[32];snprintf(number,sizeof number,"%d",payload[0]);
        execl(impl->executable.c_str(),"desk","--task-worker",number,static_cast<char *>(nullptr));_exit(127);
    }
    close(payload[0]);close(events[1]);
    if(pid<0){close(payload[1]);close(events[0]);seterr(&app,tr("无法启动任务。"));return -1;}
    fcntl(master,F_SETFD,FD_CLOEXEC);nonblocking(master);
    body+='\n';
    bool sent=write_all(payload[1],body);
    if(!sent){close(payload[1]);kill(pid,SIGTERM);while(waitpid(pid,nullptr,0)<0&&errno==EINTR){}close(master);close(events[0]);seterr(&app,tr("无法传递任务配置。"));return -1;}
    Job j;j.id=impl->next++;j.pid=pid;j.master=master;j.events=events[0];j.feed=payload[1];j.title=task;j.workspace=root;j.changed_at=monotime();j.turn_started=monotime();
    int id=j.id;impl->jobs.push_back(std::move(j));return id;
}
J *Tasks::snapshot(){
    refresh();J *items=arr();
    for(const auto &j:impl->jobs){
        if(j.retired)continue;
        J *v=obj();put(v,"id",json_object_new_int(j.id));put(v,"active",json_object_new_boolean(j.active));put(v,"state",str(tr(j.active&&j.cancelled?"正在停止":j.state.c_str())));put(v,"task",str(j.title.c_str()));put(v,"workspace",str(j.workspace.c_str()));
        put(v,"awaiting_approval",json_object_new_boolean(j.active&&!j.cancelled&&j.state=="等待确认"));
        put(v,"changed_at",json_object_new_double(j.changed_at));
        bool thinking=j.thinking&&j.active&&!j.cancelled;
        put(v,"thinking",json_object_new_boolean(thinking));
        put(v,"thinking_seconds",json_object_new_double(thinking?monotime()-j.thinking_started:0));
        put(v,"turn_seconds",json_object_new_double(j.active&&j.turn_started>0?monotime()-j.turn_started:0));
        put(v,"tokens",json_object_new_int64(static_cast<int64_t>(j.tokens)));
        add(items,v);
    }return items;
}
bool Tasks::cancel(int id){refresh();auto *j=impl->find(id);if(!j||!j->active)return false;j->cancelled=true;j->state="正在停止";j->changed_at=monotime();return kill(j->pid,SIGINT)==0;}
bool Tasks::remove(int id){
    refresh();auto *j=impl->find(id);if(!j)return false;
    j->retired=true;j->retired_at=monotime();j->cancelled=true;
    // Close an idle worker's control pipe and interrupt active operations.
    close(j->feed);j->feed=-1;
    if(j->alive)kill(j->pid,SIGTERM);
    refresh();return true;
}
bool Tasks::resume(int id,const std::string &task,J *settings){
    refresh();Job *j=impl->find(id);
    if(!j||j->active||!j->alive||task.empty()||task.size()>65536)return false;
    if(std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const Job &job){return job.active;})>=4)return false;
    JsonPtr message(obj());if(settings)put(message.get(),"settings",copy(settings));put(message.get(),"language",str(language_code()));put(message.get(),"task",str(task.c_str()));
    std::string text=json_object_to_json_string_ext(message.get(),JSON_C_TO_STRING_PLAIN);if(text.size()>250000)return false;text+='\n';
    if(!write_all(j->feed,text))return false;
    j->active=true;j->cancelled=false;j->state="执行中";j->title=task;j->changed_at=monotime();
    j->turn_started=monotime();j->tokens=0;
    return true;
}
char *Tasks::readline(const char *prompt){
    return editable_input(prompt,nullptr,false,[this]{refresh();if(on_idle)on_idle();});
}
std::string Tasks::output(int id,bool *truncated){refresh();Job *j=impl->find(id);if(!j)return {};if(truncated)*truncated=j->truncated;return j->output;}
bool Tasks::decide(int id,bool allow){refresh();Job *j=impl->find(id);return j&&j->active&&j->state=="等待确认"&&write_all(j->master,allow?"1\n":"2\n");}
bool Tasks::respond(int id,int answer){
    refresh();Job *j=impl->find(id);
    if(!j||!j->active||j->state!="等待确认"||answer<0||answer>3)return false;
    if(!write_all(j->master,std::to_string(answer)+"\n"))return false;
    j->state="执行中";
    return true;
}
int task_worker(std::intptr_t fd){
    FILE *control=fdopen(fd,"r");if(!control)return 1;
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct Control {FILE *file;char *line=nullptr;size_t capacity=0;~Control(){free(line);fclose(file);}} source{control};
    auto read_message=[&]()->JsonPtr{
        ssize_t n=getline(&source.line,&source.capacity,control);
        if(n<=0||n>256000)return JsonPtr{};
        return JsonPtr(json_tokener_parse(source.line));
    };
    JsonPtr data=read_message();if(!data)return 1;
    set_language(sget(data.get(),"language"));
    App app;
    app.guided_approval=bget(data.get(),"guided_approval");
    app.conversation_worker=bget(data.get(),"conversation");app.config=copy(get(data.get(),"config"));
    snprintf(app.root,sizeof app.root,"%s",sget(data.get(),"root"));
    snprintf(app.config_dir,sizeof app.config_dir,"%s",sget(data.get(),"config_dir"));
    app.yes=bget(data.get(),"yes");app.auto_edit=bget(data.get(),"auto_edit");app.shell=bget(data.get(),"shell");app.desktop=bget(data.get(),"desktop");app.record=bget(data.get(),"record");app.verbose=bget(data.get(),"verbose");
    app.max_steps=iget(data.get(),"max_steps",20);app.task_events=iget(data.get(),"events",-1);
    if(app.task_events>=0)fcntl(app.task_events,F_SETFD,FD_CLOEXEC);
    auto cleanup=[](App *a){app_cleanup(a);};std::unique_ptr<App,decltype(cleanup)> guard(&app,cleanup);
    std::string task=sget(data.get(),"task");
    app.conversation_id=sget(data.get(),"conversation_id");
    if(!app.conversation_id.empty())app.messages=history_context(app,app.conversation_id);
    for(;;){
        if(interrupted==SIGTERM)break;
        interrupted=0;
        int rc=run_task(&app,task.c_str());
        if(rc)fprintf(stderr,"%s\n",app.error);
        fflush(nullptr);
        desktop_clear(&app);
        task_event(app,interrupted||!strcmp(app.state,"已取消")?'c':rc?'f':'d');
        if(interrupted==SIGTERM)break;
        interrupted=0;
        JsonPtr next=read_message();if(!next||interrupted)break;
        apply_conversation_settings(app,get(next.get(),"settings"));set_language(sget(next.get(),"language"));
        task=sget(next.get(),"task");if(task.empty())break;
    }
    return 0;
}
