#ifndef DESK_TASKS_HPP
#define DESK_TASKS_HPP
#include "desk.hpp"
#include <cstring>
class Tasks {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Tasks(std::string executable="/proc/self/exe");
    ~Tasks();
    int start(App &app,const std::string &task,const std::string &workspace="");
    void refresh();
    std::uint64_t tokens();
    J *snapshot();
    void list();
    bool cancel(int id);
    bool resume(int id,const std::string &task);
    void attach(int id);
    std::string output(int id,bool *truncated=nullptr);
    bool decide(int id,bool allow);
    char *readline(const char *prompt="\n› ");
};
class ResourceLock {
    std::intptr_t fd=-1;
public:
    ResourceLock(App &app,bool desktop);
    ~ResourceLock();
    ResourceLock(const ResourceLock &)=delete;
    ResourceLock &operator=(const ResourceLock &)=delete;
    bool acquired() const { return fd>=0; }
};
inline std::uint64_t add_tokens(std::uint64_t total,std::uint64_t amount){
    return amount>UINT64_MAX-total?UINT64_MAX:total+amount;
}
struct TaskUsage {
    std::uint64_t total=0;
    char bytes[sizeof(std::uint64_t)]{};
    size_t remaining=0;

    // A usage event is the marker 'u' followed by one native uint64_t.
    bool consume(char byte){
        if(remaining){
            bytes[sizeof bytes-remaining]=byte;
            if(--remaining==0){
                std::uint64_t amount;
                std::memcpy(&amount,bytes,sizeof amount);
                total=add_tokens(total,amount);
            }
            return true;
        }
        if(byte!='u')return false;
        remaining=sizeof bytes;
        return true;
    }
};
void task_event(App &app,std::string_view bytes);
inline void task_event(App &app,char state){task_event(app,std::string_view(&state,1));}

int task_worker(std::intptr_t fd);
#endif
