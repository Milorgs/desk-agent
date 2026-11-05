#ifndef DESK_H
#define DESK_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#define VERSION "1.2.1"
#define CONTEXT_LIMIT 128000
#define FILE_LIMIT 65536
#define RESPONSE_LIMIT 4000000
using J = json_object;
struct JsonDeleter {
    void operator()(J *value) const noexcept { json_object_put(value); }
};
using JsonPtr = std::unique_ptr<J,JsonDeleter>;
struct App {
    J *store=nullptr, *config=nullptr, *messages=nullptr;
    char root[PATH_MAX]{}, config_dir[PATH_MAX]{}, error[512]{};
    bool yes=false, shell=true, desktop=false, record=false;
    int max_steps=20;
    int task_events=-1;
    const char *state="等待任务";
    void *desktop_data=nullptr;
};
extern volatile sig_atomic_t interrupted;
J *obj(void);
J *arr(void);
J *str(const char *s);
J *copy(J *j);
J *get(J *j,const char *key);
const char *sget(J *j,const char *key);
size_t alen(J *j);
J *at(J *j,size_t index);
int iget(J *j,const char *key,int fallback);
bool bget(J *j,const char *key);
bool tool_failed(J *result);
bool valid_utf8(std::string_view text) noexcept;
void put(J *j,const char *key,J *value);
void add(J *j,J *value);
J *err(const char *message);
void seterr(App *a,const char *message);
char *read_bytes(const char *path,size_t limit,size_t *length);
int path_suffix(char out[PATH_MAX],const char *base,const char *suffix);
int mkdirs(const char *path,mode_t mode);
int atomic_file(const char *path,const void *data,size_t len,mode_t mode);
char *input(const char *prompt,const char *fallback,bool secret);
int approve(App *a,const char *description);
void id_string(char out[33]);
double monotime(void);
int store_load(App *a);
int config_valid(App *a,J *config);
J *config_default(const char *name);
int save_config(App *a,J *config);
int activate(App *a,const char *name);
int config_name_valid(const char *name);
int api_check(App *a,J *config);
J *api_models(App *a,J *config);
J *api_complete(App *a,J *config,J *messages,J *tools);
J *native_payload(J *config,J *messages,J *tools);
J *native_response(App *a,J *data);
J *api_request(App *a,J *config,const char *endpoint,J *payload);
J *schemas(App *a);
J *tool_call(App *a,const char *name,J *arguments);
J *run_command(App *a,const char *command,int timeout_ms);
int resolve_file(App *a,const char *path,char out[PATH_MAX]);
char *sha256(const void *bytes,size_t len);
J *desktop_snapshot(App *a,const char *app);
J *desktop_act(App *a,const char *ref,const char *action,const char *text);
void desktop_clear(App *a);
bool desktop_compiled(void);
void reset_session(App *a);
int run_task(App *a,const char *task);
J *context(App *a,J *tools);
void save_record(App *a);
void app_cleanup(App *a);
#endif
