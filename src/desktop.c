#include "desk.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef WITH_ATSPI
#include <atspi/atspi.h>
typedef struct{
    AtspiAccessible *nodes[180];
    char *names[180];
    AtspiRole roles[180];
    int count;
    char stamp[33];
    double created;
    bool ready;
}
Desktop;
static Desktop *desktop(App *a){
    if(!a->desktop_data)a->desktop_data=calloc(1,sizeof(Desktop));
    return a->desktop_data;
}
void desktop_clear(App *a){
    Desktop *d=a->desktop_data;
    if(!d)return;
    for(int i=0;i<d->count;i++){
        g_object_unref(d->nodes[i]);
        g_free(d->names[i]);
    }
    d->count=0;
    d->created=0;
}
static void visit(Desktop *d,AtspiAccessible *node,int depth,J *nodes){
    if(!node||depth>10||d->count>=180||monotime()-d->created>8||interrupted)return;
    AtspiRole role=atspi_accessible_get_role(node,NULL);
    if(role==ATSPI_ROLE_PASSWORD_TEXT)return;
    char *name=atspi_accessible_get_name(node,NULL),*role_name=atspi_accessible_get_role_name(node,NULL);
    int index=d->count++;
    d->nodes[index]=g_object_ref(node);
    d->names[index]=g_strdup(name?name:"");
    d->roles[index]=role;
    char ref[64];
    snprintf(ref,sizeof ref,"%s:%d",d->stamp,index+1);
    J *item=obj(),*actions=arr();
    put(item,"ref",str(ref));
    put(item,"name",str(name));
    put(item,"role",str(role_name));
    put(item,"depth",json_object_new_int(depth));
    put(item,"actions",actions);
    AtspiStateSet *states=atspi_accessible_get_state_set(node);
    put(item,"enabled",json_object_new_boolean(states&&atspi_state_set_contains(states,ATSPI_STATE_ENABLED)));
    put(item,"editable",json_object_new_boolean(states&&atspi_state_set_contains(states,ATSPI_STATE_EDITABLE)));
    if(states)g_object_unref(states);
    AtspiAction *action=atspi_accessible_get_action_iface(node);
    if(action){
        int n=atspi_action_get_n_actions(action,NULL);
        for(int i=0;i<n&&i<16;i++){
            char *value=atspi_action_get_action_name(action,i,NULL);
            add(actions,str(value));
            g_free(value);
        }
        g_object_unref(action);
    }
    AtspiText *text=atspi_accessible_get_text_iface(node);
    if(text){
        int n=atspi_text_get_character_count(text,NULL);
        if(n>1200)n=1200;
        if(n>0){
            char *value=atspi_text_get_text(text,0,n,NULL);
            put(item,"text",str(value));
            g_free(value);
        }
        g_object_unref(text);
    }
    add(nodes,item);
    g_free(name);
    g_free(role_name);
    int count=atspi_accessible_get_child_count(node,NULL);
    for(int i=0;i<count&&i<180&&d->count<180;i++){
        AtspiAccessible *child=atspi_accessible_get_child_at_index(node,i,NULL);
        if(child){
            visit(d,child,depth+1,nodes);
            g_object_unref(child);
        }
    }
}
J *desktop_snapshot(App *a,const char *app){
    Desktop *d=desktop(a);
    if(!d)return err("内存不足。");
    desktop_clear(a);
    if(!d->ready){
        if(atspi_init()!=0)return err("AT-SPI 无法初始化，请确认正在图形会话中运行。");
        atspi_set_timeout(1500,3000);
        d->ready=true;
    }
    id_string(d->stamp);
    d->created=monotime();
    AtspiAccessible *root=atspi_get_desktop(0);
    if(!root)return err("无法读取桌面无障碍服务。");
    J *r=obj(),*apps=arr(),*nodes=arr();
    put(r,"apps",apps);
    put(r,"nodes",nodes);
    put(r,"app",str(app));
    int n=atspi_accessible_get_child_count(root,NULL);
    for(int i=0;i<n&&i<80&&!interrupted;i++){
        AtspiAccessible *application=atspi_accessible_get_child_at_index(root,i,NULL);
        if(!application)continue;
        char *name=atspi_accessible_get_name(application,NULL);
        add(apps,str(name));
        if(*app&&name&&!g_utf8_collate(app,name))visit(d,application,0,nodes);
        g_free(name);
        g_object_unref(application);
    }
    g_object_unref(root);
    put(r,"partial",json_object_new_boolean(d->count>=180||monotime()-d->created>=8));
    put(r,"hint",str("按完整应用名读取控件。每次操作后重新 snapshot 验证。"));
    return r;
}
J *desktop_act(App *a,const char *ref,const char *action,const char *text){
    Desktop *d=desktop(a);
    if(!d||monotime()-d->created>30||strncmp(ref,d->stamp,32)||ref[32]!=':')return err("控件引用无效或过期，请重新读取桌面。");
    char *end=NULL;
    long number=strtol(ref+33,&end,10);
    if(!end||*end||number<1||number>d->count)return err("控件引用无效。");
    int i=(int)number-1;
    AtspiAccessible *node=g_object_ref(d->nodes[i]);
    char *name=atspi_accessible_get_name(node,NULL);
    bool same=name&&!strcmp(name,d->names[i])&&atspi_accessible_get_role(node,NULL)==d->roles[i];
    g_free(name);
    desktop_clear(a);
    if(!same){
        g_object_unref(node);
        return err("控件已变化。");
    }
    AtspiStateSet *states=atspi_accessible_get_state_set(node);
    bool enabled=states&&atspi_state_set_contains(states,ATSPI_STATE_ENABLED);
    bool editable=states&&atspi_state_set_contains(states,ATSPI_STATE_EDITABLE);
    if(states)g_object_unref(states);
    bool ok=false;
    if(enabled&&text&&editable&&atspi_accessible_get_role(node,NULL)!=ATSPI_ROLE_PASSWORD_TEXT){
        AtspiEditableText *t=atspi_accessible_get_editable_text_iface(node);
        if(t){
            ok=atspi_editable_text_set_text_contents(t,text,NULL);
            g_object_unref(t);
        }
    }
    else if(enabled&&action){
        AtspiAction *act=atspi_accessible_get_action_iface(node);
        if(act){
            int n=atspi_action_get_n_actions(act,NULL);
            for(int k=0;k<n;k++){
                char *v=atspi_action_get_action_name(act,k,NULL);
                if(v&&!strcmp(v,action))ok=atspi_action_do_action(act,k,NULL);
                g_free(v);
                if(ok)break;
            }
            g_object_unref(act);
        }
    }
    g_object_unref(node);
    J *r=obj();
    put(r,"accepted",json_object_new_boolean(ok));
    put(r,"verified",json_object_new_boolean(0));
    put(r,"next",str("重新读取桌面验证结果。"));
    return r;
}
bool desktop_compiled(void){
    return true;
}
#else
void desktop_clear(App *a){
    (void)a;
}
bool desktop_compiled(void){
    return false;
}
J *desktop_snapshot(App *a,const char *name){
    (void)a;
    (void)name;
    return err("本次构建未启用 AT-SPI，请安装开发库后重新编译。");
}
J *desktop_act(App *a,const char *r,const char *action,const char *text){
    (void)a;
    (void)r;
    (void)action;
    (void)text;
    return err("本次构建未启用 AT-SPI。");
}
#endif
