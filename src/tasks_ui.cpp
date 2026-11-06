#include "tasks.hpp"

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
    bool reset=bget(settings,"reset")||!same_model(app.config,get(settings,"config"));
    desktop_clear(&app);
    json_object_put(app.config);app.config=copy(get(settings,"config"));
    app.yes=bget(settings,"yes");app.auto_edit=bget(settings,"auto_edit");
    app.shell=bget(settings,"shell");app.desktop=bget(settings,"desktop");
    app.record=bget(settings,"record");app.verbose=bget(settings,"verbose");
    app.max_steps=iget(settings,"max_steps",20);
    if(reset)reset_session(&app);
}
