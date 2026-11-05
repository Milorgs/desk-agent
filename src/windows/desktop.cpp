#include "win.hpp"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace {
struct Reference {ComPtr<IUIAutomationElement> element;std::string name;CONTROLTYPEID role=0;};
struct Desktop {
    ComPtr<IUIAutomation> api;ComPtr<IUIAutomationTreeWalker> walker;
    std::vector<Reference> references;std::string stamp;double created=0;bool com=false;
    ~Desktop(){references.clear();walker.Reset();api.Reset();if(com)CoUninitialize();}
};
std::string name_of(IUIAutomationElement *element){BSTR name=nullptr;if(FAILED(element->get_CurrentName(&name)))return {};std::string result;try{if(name)result=utf8(std::wstring_view(name,SysStringLen(name)));}catch(...){SysFreeString(name);throw;}SysFreeString(name);return result;}
Desktop *state(App *app){
    if(app->desktop_data)return static_cast<Desktop *>(app->desktop_data);
    auto data=std::make_unique<Desktop>();HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE)return nullptr;data->com=SUCCEEDED(hr);
    if(FAILED(CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&data->api)))||FAILED(data->api->get_ControlViewWalker(&data->walker)))return nullptr;
    ComPtr<IUIAutomation2> timeout;if(SUCCEEDED(data->api.As(&timeout))){timeout->put_ConnectionTimeout(1500);timeout->put_TransactionTimeout(3000);}
    app->desktop_data=data.release();return static_cast<Desktop *>(app->desktop_data);
}
bool password(IUIAutomationElement *element){BOOL value=TRUE;return FAILED(element->get_CurrentIsPassword(&value))||value;}
void visit(Desktop &data,IUIAutomationElement *element,int depth,J *items){
    if(interrupted||depth>10||data.references.size()>=180||monotime()-data.created>8||password(element))return;
    std::string name=name_of(element);CONTROLTYPEID role=0;if(FAILED(element->get_CurrentControlType(&role)))return;
    Reference reference;reference.element=element;reference.name=name;reference.role=role;data.references.push_back(std::move(reference));
    std::string ref=data.stamp+":"+std::to_string(data.references.size());J *node=obj(),*actions=arr();put(node,"ref",str(ref.c_str()));put(node,"name",str(name.c_str()));put(node,"control_type",json_object_new_int(role));put(node,"depth",json_object_new_int(depth));put(node,"actions",actions);
    BSTR localized=nullptr;if(SUCCEEDED(element->get_CurrentLocalizedControlType(&localized))&&localized){auto value=utf8(localized);put(node,"role",str(value.c_str()));}SysFreeString(localized);
    BOOL enabled=FALSE;element->get_CurrentIsEnabled(&enabled);put(node,"enabled",json_object_new_boolean(enabled));
    ComPtr<IUIAutomationInvokePattern> invoke;if(SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&invoke))))add(actions,str("invoke"));
    ComPtr<IUIAutomationTogglePattern> toggle;if(SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(&toggle))))add(actions,str("toggle"));
    ComPtr<IUIAutomationSelectionItemPattern> select;if(SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&select))))add(actions,str("select"));
    ComPtr<IUIAutomationValuePattern> value;BOOL readonly=TRUE;
    if(SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&value)))){
        value->get_CurrentIsReadOnly(&readonly);BSTR text=nullptr;
        if(SUCCEEDED(value->get_CurrentValue(&text))&&text){std::wstring excerpt(text,SysStringLen(text));if(excerpt.size()>1200)excerpt.resize(1200);if(!excerpt.empty()&&excerpt.back()>=0xd800&&excerpt.back()<=0xdbff)excerpt.pop_back();auto bytes=utf8(excerpt);put(node,"text",str(bytes.c_str()));}SysFreeString(text);
    }
    put(node,"editable",json_object_new_boolean(!readonly));add(items,node);
    ComPtr<IUIAutomationElement> child;if(FAILED(data.walker->GetFirstChildElement(element,&child)))return;
    for(int count=0;child&&count<180&&data.references.size()<180&&monotime()-data.created<=8;count++){
        visit(data,child.Get(),depth+1,items);ComPtr<IUIAutomationElement> next;data.walker->GetNextSiblingElement(child.Get(),&next);child=std::move(next);
    }
}
}
void desktop_clear(App *app){auto *data=static_cast<Desktop *>(app->desktop_data);if(data){data->references.clear();data->created=0;}}
void desktop_destroy(App *app){delete static_cast<Desktop *>(app->desktop_data);app->desktop_data=nullptr;}
bool desktop_compiled(){return true;}
J *desktop_snapshot(App *app,const char *target){
    Desktop *data=state(app);if(!data)return err("Windows UI Automation could not be initialized.");desktop_clear(app);
    char stamp[33];id_string(stamp);data->stamp=stamp;data->created=monotime();
    ComPtr<IUIAutomationElement> root;if(FAILED(data->api->GetRootElement(&root)))return err("Could not read the Windows desktop.");
    J *result=obj(),*apps=arr(),*nodes=arr();put(result,"apps",apps);put(result,"nodes",nodes);put(result,"app",str(target));
    ComPtr<IUIAutomationElement> window;data->walker->GetFirstChildElement(root.Get(),&window);bool found=!*target;
    for(int count=0;window&&count<80&&!interrupted&&monotime()-data->created<8;count++){
        if(!password(window.Get())){std::string name=name_of(window.Get());if(!name.empty())add(apps,str(name.c_str()));if(*target&&name==target){found=true;visit(*data,window.Get(),0,nodes);}}
        ComPtr<IUIAutomationElement> next;data->walker->GetNextSiblingElement(window.Get(),&next);window=std::move(next);
    }
    put(result,"partial",json_object_new_boolean(data->references.size()>=180||monotime()-data->created>=8));
    put(result,"hint",str("Use an exact window title from apps. Read a fresh snapshot after every action."));
    if(!found)put(result,"error",str("Application window not found. Read the current window titles again."));return result;
}
J *desktop_act(App *app,const char *ref,const char *action,const char *text){
    auto *data=static_cast<Desktop *>(app->desktop_data);if(!data||monotime()-data->created>30)return err(tr("控件引用无效或过期，请重新读取桌面。"));
    std::string prefix=data->stamp+":";if(strncmp(ref,prefix.c_str(),prefix.size()))return err(tr("控件引用无效。"));char *end=nullptr;long index=strtol(ref+prefix.size(),&end,10);
    if(*end||index<1||static_cast<size_t>(index)>data->references.size())return err(tr("控件引用无效。"));
    auto entry=data->references[static_cast<size_t>(index-1)];BOOL enabled=FALSE;CONTROLTYPEID role=0;
    bool same=SUCCEEDED(entry.element->get_CurrentControlType(&role))&&role==entry.role&&!password(entry.element.Get())&&name_of(entry.element.Get())==entry.name;
    entry.element->get_CurrentIsEnabled(&enabled);desktop_clear(app);if(!same)return err(tr("控件已变化。"));
    HRESULT accepted=E_ACCESSDENIED;
    if(enabled&&text){ComPtr<IUIAutomationValuePattern> value;BOOL readonly=TRUE;if(SUCCEEDED(entry.element->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&value)))&&SUCCEEDED(value->get_CurrentIsReadOnly(&readonly))&&!readonly){auto replacement=wide(text);accepted=value->SetValue(replacement.c_str());}}
    else if(enabled&&action){
        if(!strcmp(action,"invoke")){ComPtr<IUIAutomationInvokePattern> pattern;if(SUCCEEDED(entry.element->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&pattern))))accepted=pattern->Invoke();}
        else if(!strcmp(action,"toggle")){ComPtr<IUIAutomationTogglePattern> pattern;if(SUCCEEDED(entry.element->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(&pattern))))accepted=pattern->Toggle();}
        else if(!strcmp(action,"select")){ComPtr<IUIAutomationSelectionItemPattern> pattern;if(SUCCEEDED(entry.element->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&pattern))))accepted=pattern->Select();}
    }
    J *result=obj();put(result,"accepted",json_object_new_boolean(SUCCEEDED(accepted)));put(result,"verified",json_object_new_boolean(false));put(result,"next",str(tr("重新读取桌面验证结果。")));return result;
}
