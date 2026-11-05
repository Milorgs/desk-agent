#include "tasks.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
void Tasks::attach(int id){
    bool show=true;
    for(;;){
        JsonPtr items(snapshot());J *job=nullptr;
        for(size_t i=0;i<alen(items.get());i++)if(iget(at(items.get(),i),"id",0)==id)job=at(items.get(),i);
        if(!job){puts(tr("任务编号不存在。"));return;}
        printf(tr("\n任务 [%d]：%s · %s\n"),id,sget(job,"task"),sget(job,"state"));
        if(show){bool truncated=false;auto text=output(id,&truncated);if(truncated)puts(tr("…仅保留最近 64 KB 输出。"));fwrite(text.data(),1,text.size(),stdout);show=false;}
        puts(tr("\n  [1] 刷新输出\n  [2] 确认待处理操作\n  [3] 继续此任务\n  [4] 停止此任务\n  [0] 返回"));
        std::unique_ptr<char,decltype(&free)> text(readline(tr("请选择编号：")),free);if(!text||interrupted)return;
        std::string choice=text.get();
        if(choice=="0")return;
        if(choice=="1")show=true;
        else if(choice=="2"){
            items.reset(snapshot());job=nullptr;for(size_t i=0;i<alen(items.get());i++)if(iget(at(items.get(),i),"id",0)==id)job=at(items.get(),i);
            if(!job||!bget(job,"active")||strcmp(sget(job,"state"),tr("等待确认"))){puts(tr("当前没有待确认的操作。"));continue;}
            bool truncated=false;auto preview=output(id,&truncated);if(truncated)puts(tr("…仅保留最近 64 KB 输出。"));fwrite(preview.data(),1,preview.size(),stdout);
            puts(tr("\n本次操作\n  [1] 允许\n  [2] 拒绝\n  [0] 返回"));
            std::unique_ptr<char,decltype(&free)> answer(readline(tr("请选择编号：")),free);
            if(answer&&!strcmp(answer.get(),"1"))decide(id,true);else if(answer&&!strcmp(answer.get(),"2"))decide(id,false);
        }else if(choice=="3"){
            items.reset(snapshot());job=nullptr;for(size_t i=0;i<alen(items.get());i++)if(iget(at(items.get(),i),"id",0)==id)job=at(items.get(),i);
            if(!job||bget(job,"active")){puts(tr("请等待此任务结束后再继续。"));continue;}
            std::unique_ptr<char,decltype(&free)> task(readline(tr("新需求（回车返回）：")),free);
            if(task&&*task)puts(resume(id,task.get())?tr("已继续该任务，保留原模型和对话。"):tr("无法继续此任务，或当前活动任务已满。"));
        }else if(choice=="4")puts(cancel(id)?tr("已请求停止该任务。"):tr("任务不存在或已经结束。"));
        else puts(tr("请输入列表中的编号。"));
    }
}


// Conversation workers retain their context, while explicit model changes reset it.
J *conversation_settings(const App &app,bool reset){
    J *settings=obj();
    put(settings,"config",copy(app.config));
    put(settings,"reset",json_object_new_boolean(reset));
    put(settings,"yes",json_object_new_boolean(app.yes));
    put(settings,"auto_edit",json_object_new_boolean(app.auto_edit));
    put(settings,"shell",json_object_new_boolean(app.shell));
    put(settings,"desktop",json_object_new_boolean(app.desktop));
    put(settings,"record",json_object_new_boolean(app.record));
    put(settings,"verbose",json_object_new_boolean(app.verbose));
    put(settings,"max_steps",json_object_new_int(app.max_steps));
    return settings;
}
void apply_conversation_settings(App &app,J *settings){
    if(!settings)return;
    desktop_clear(&app);
    json_object_put(app.config);app.config=copy(get(settings,"config"));
    app.yes=bget(settings,"yes");app.auto_edit=bget(settings,"auto_edit");
    app.shell=bget(settings,"shell");app.desktop=bget(settings,"desktop");
    app.record=bget(settings,"record");app.verbose=bget(settings,"verbose");
    app.max_steps=iget(settings,"max_steps",20);
    if(bget(settings,"reset"))reset_session(&app);
}
