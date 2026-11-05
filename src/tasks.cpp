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
        if(app.task_events<0){puts(tr("资源正被任务占用，请进入任务管理处理待确认操作，或新建并行任务。"));break;}
        if(!waiting){puts(desktop?tr("等待其他任务释放桌面…"):tr("等待同一工作目录的其他操作结束…"));fflush(stdout);task_event(app,'q');waiting=true;}
        usleep(50000);
    }
    close(candidate);
}
ResourceLock::~ResourceLock(){if(fd>=0){flock(fd,LOCK_UN);close(fd);}}
struct Tasks::Impl {
    std::vector<Job> jobs;
    Impl(){jobs.reserve(32);}
    int next=1;
    std::string executable;
    Job *find(int id){for(auto &j:jobs)if(j.id==id)return &j;return nullptr;}
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
        while((n=read(j.events,events,sizeof events))>0)for(ssize_t k=0;k<n;k++){
            if(events[k]=='d'){j.state="已完成";j.active=false;}
            else if(events[k]=='f'){j.state="失败";j.active=false;}
            else if(events[k]=='c'){j.state="已取消";j.active=false;}
            else if(events[k]=='a')j.state="等待确认";
            else if(events[k]=='q')j.state="等待资源";
            else if(events[k]=='r')j.state="执行中";
        }
        drain(j);
        if(!j.alive)continue;
        int status=0;
        pid_t ended=waitpid(j.pid,&status,WNOHANG);
        if(ended==j.pid){
            j.alive=false;
            if(j.active)j.state=j.cancelled||(WIFEXITED(status)&&WEXITSTATUS(status)==2)?"已取消":WIFEXITED(status)&&WEXITSTATUS(status)==0?"已完成":"失败";
            j.active=false;
            drain(j);
        }
    }
}
int Tasks::start(App &app,const std::string &task,const std::string &workspace){
    refresh();
    if(task.empty()||task.size()>65536||impl->jobs.size()>=32||std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const Job &j){return j.active;})>=4){
        seterr(&app,tr("任务为空、过长或达到上限（同时 4 个，本次会话共 32 个）。"));return -1;
    }
    if(config_valid(&app,app.config))return -1;
    char root[PATH_MAX];
    struct stat directory{};
    if(!realpath(workspace.empty()?app.root:workspace.c_str(),root)||stat(root,&directory)||!S_ISDIR(directory.st_mode)){seterr(&app,tr("工作目录不存在。"));return -1;}
    int payload[2],events[2];
    if(pipe2(payload,O_CLOEXEC)){seterr(&app,tr("无法创建任务管道。"));return -1;}
    if(pipe2(events,O_CLOEXEC|O_NONBLOCK)){close(payload[0]);close(payload[1]);seterr(&app,tr("无法创建状态管道。"));return -1;}
    JsonPtr data(obj());
    put(data.get(),"language",str(language_code()));put(data.get(),"config",copy(app.config));put(data.get(),"root",str(root));put(data.get(),"config_dir",str(app.config_dir));
    put(data.get(),"task",str(task.c_str()));put(data.get(),"yes",json_object_new_boolean(app.yes));
    put(data.get(),"shell",json_object_new_boolean(app.shell));put(data.get(),"desktop",json_object_new_boolean(app.desktop));
    put(data.get(),"record",json_object_new_boolean(app.record));put(data.get(),"max_steps",json_object_new_int(app.max_steps));
    put(data.get(),"events",json_object_new_int(events[1]));
    std::string body=json_object_to_json_string_ext(data.get(),JSON_C_TO_STRING_PLAIN);
    if(body.size()>250000){close(payload[0]);close(payload[1]);close(events[0]);close(events[1]);seterr(&app,tr("任务配置过大。"));return -1;}
    // Flush inherited stdio; a worker execs a fresh process, including AT-SPI.
    fflush(nullptr);
    int master=-1;
    pid_t controller=getpid();
    pid_t pid=forkpty(&master,nullptr,nullptr,nullptr);
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
    Job j;j.id=impl->next++;j.pid=pid;j.master=master;j.events=events[0];j.feed=payload[1];j.title=task;j.workspace=root;
    int id=j.id;impl->jobs.push_back(std::move(j));return id;
}
J *Tasks::snapshot(){
    refresh();J *items=arr();
    for(const auto &j:impl->jobs){J *v=obj();put(v,"id",json_object_new_int(j.id));put(v,"active",json_object_new_boolean(j.active));put(v,"state",str(tr(j.state.c_str())));put(v,"task",str(j.title.c_str()));put(v,"workspace",str(j.workspace.c_str()));add(items,v);}return items;
}
void Tasks::list(){
    refresh();if(impl->jobs.empty())puts(tr("尚无任务，请选择 [1] 新建并行任务。"));
    for(const auto &j:impl->jobs)printf("[%d] %s · %.120s\n    %s\n",j.id,tr(j.state.c_str()),j.title.c_str(),j.workspace.c_str());
}
bool Tasks::cancel(int id){refresh();auto *j=impl->find(id);if(!j||!j->active)return false;j->cancelled=true;j->state="正在停止";return kill(j->pid,SIGINT)==0;}
bool Tasks::resume(int id,const std::string &task){
    refresh();Job *j=impl->find(id);
    if(!j||j->active||!j->alive||task.empty()||task.size()>65536)return false;
    if(std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const Job &job){return job.active;})>=4)return false;
    JsonPtr message(obj());put(message.get(),"language",str(language_code()));put(message.get(),"task",str(task.c_str()));
    std::string text=json_object_to_json_string_ext(message.get(),JSON_C_TO_STRING_PLAIN);text+='\n';
    if(!write_all(j->feed,text))return false;
    j->active=true;j->cancelled=false;j->state="执行中";j->title=task;
    return true;
}
char *Tasks::readline(const char *prompt){
    if(!isatty(STDIN_FILENO))return input(prompt,nullptr,false);
    fputs(prompt,stdout);fflush(stdout);
    for(;;){
        refresh();
        std::vector<pollfd> fds{{STDIN_FILENO,POLLIN,0}};
        for(const auto &j:impl->jobs)if(j.alive){fds.push_back({j.master,POLLIN,0});fds.push_back({j.events,POLLIN,0});}
        int ready=poll(fds.data(),fds.size(),200);
        if(interrupted)return nullptr;
        if(ready<0&&errno!=EINTR)return nullptr;
        if(fds[0].revents&(POLLIN|POLLHUP))return input("",nullptr,false);
    }
}
void Tasks::attach(int id){
    Job *j=impl->find(id);if(!j){puts(tr("任务编号不存在。"));return;}
    bool first=true;
    for(;;){
        refresh();
        printf(tr("\n任务 [%d]：%s · %s\n"),id,j->title.c_str(),tr(j->state.c_str()));
        if(first){if(j->truncated)puts(tr("…仅保留最近 64 KB 输出。"));fwrite(j->output.data(),1,j->output.size(),stdout);first=false;}
        puts(tr("\n  [1] 刷新输出\n  [2] 确认待处理操作\n  [3] 继续此任务\n  [4] 停止此任务\n  [0] 返回"));
        std::unique_ptr<char,decltype(&free)> text(readline(tr("请选择编号：")),free);
        if(!text||interrupted)return;
        std::string choice=text.get();refresh();
        if(choice=="0")return;
        if(choice=="1")first=true;
        else if(choice=="2"){
            if(!j->active||j->state!="等待确认"){puts(tr("当前没有待确认的操作。"));continue;}
            if(j->truncated)puts(tr("…仅保留最近 64 KB 输出。"));
            fwrite(j->output.data(),1,j->output.size(),stdout);
            puts(tr("\n本次操作\n  [1] 允许\n  [2] 拒绝\n  [0] 返回"));
            std::unique_ptr<char,decltype(&free)> answer(readline(tr("请选择编号：")),free);
            if(answer&&j->active&&j->state=="等待确认"){
                if(!strcmp(answer.get(),"1"))write_all(j->master,"1\n");
                else if(!strcmp(answer.get(),"2"))write_all(j->master,"2\n");
            }
        }else if(choice=="3"){
            if(j->active){puts(tr("请等待此任务结束后再继续。"));continue;}
            std::unique_ptr<char,decltype(&free)> task(readline(tr("新需求（回车返回）：")),free);
            if(task&&*task){if(resume(id,task.get()))puts(tr("已继续该任务，保留原模型和对话。"));else puts(tr("无法继续此任务，或当前活动任务已满。"));}
        }else if(choice=="4")puts(cancel(id)?tr("已请求停止该任务。"):tr("任务不存在或已经结束。"));
        else puts(tr("请输入列表中的编号。"));
    }
}
int task_worker(int fd){
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
    app.config=copy(get(data.get(),"config"));
    snprintf(app.root,sizeof app.root,"%s",sget(data.get(),"root"));
    snprintf(app.config_dir,sizeof app.config_dir,"%s",sget(data.get(),"config_dir"));
    app.yes=bget(data.get(),"yes");app.shell=bget(data.get(),"shell");app.desktop=bget(data.get(),"desktop");app.record=bget(data.get(),"record");
    app.max_steps=iget(data.get(),"max_steps",20);app.task_events=iget(data.get(),"events",-1);
    if(app.task_events>=0)fcntl(app.task_events,F_SETFD,FD_CLOEXEC);
    auto cleanup=[](App *a){app_cleanup(a);};std::unique_ptr<App,decltype(cleanup)> guard(&app,cleanup);
    std::string task=sget(data.get(),"task");
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
        set_language(sget(next.get(),"language"));
        task=sget(next.get(),"task");if(task.empty())break;
    }
    return 0;
}
