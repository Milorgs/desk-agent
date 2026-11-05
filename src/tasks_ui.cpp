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
