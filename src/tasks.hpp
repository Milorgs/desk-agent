#ifndef DESK_TASKS_HPP
#define DESK_TASKS_HPP
#include "desk.hpp"
class Tasks {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Tasks(std::string executable="/proc/self/exe");
    ~Tasks();
    int start(App &app,const std::string &task,const std::string &workspace="");
    void refresh();
    J *snapshot();
    void list();
    bool cancel(int id);
    bool resume(int id,const std::string &task);
    void attach(int id);
    char *readline();
};
class ResourceLock {
    int fd=-1;
public:
    ResourceLock(App &app,bool desktop);
    ~ResourceLock();
    ResourceLock(const ResourceLock &)=delete;
    ResourceLock &operator=(const ResourceLock &)=delete;
    bool acquired() const { return fd>=0; }
};
void task_event(App &app,char state);
int task_worker(int fd);
#endif
