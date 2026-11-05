#ifndef DESK_TASKS_HPP
#define DESK_TASKS_HPP
#include "desk.hpp"
class Tasks {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    std::function<void()> on_idle;
    explicit Tasks(std::string executable="/proc/self/exe");
    ~Tasks();
    int start(App &app,const std::string &task,const std::string &workspace="");
    void refresh();
    J *snapshot();
    bool cancel(int id);
    bool resume(int id,const std::string &task,J *settings=nullptr);
    bool respond(int id,int answer);
    std::string output(int id,bool *truncated=nullptr);
    bool decide(int id,bool allow);
    char *readline(const char *prompt="\n› ");
};
class ResourceLock {
    std::intptr_t fd=-1;
public:
    ResourceLock(App &app,bool desktop);
    explicit ResourceLock(const char *config_dir);
    ~ResourceLock();
    ResourceLock(const ResourceLock &)=delete;
    ResourceLock &operator=(const ResourceLock &)=delete;
    bool acquired() const { return fd>=0; }
};
void task_event(App &app,char state);
int task_worker(std::intptr_t fd);
J *conversation_settings(const App &app,bool reset);
void apply_conversation_settings(App &app,J *settings);
#endif
