#include "desk.hpp"
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
extern char **environ;
static bool private_variable(const char *entry){
    const char *equals=strchr(entry,'=');
    if(!equals)return true;
    size_t n=(size_t)(equals-entry);
    char *name=strndup(entry,n);
    if(!name)return true;
    for(char *p=name;*p;p++)*p=(char)toupper((unsigned char)*p);
    bool secret=strstr(name,"KEY")||strstr(name,"TOKEN")||strstr(name,"SECRET")||strstr(name,"PASSWORD");
    free(name);
    return secret;
}
J *run_command(App *a,const char *command,int timeout_ms){
    if(timeout_ms<1||timeout_ms>300000)return err("命令超时设置无效。");
    int count=0;
    while(environ[count])count++;
    std::vector<char *> env;
    env.reserve(static_cast<size_t>(count)+1);
    for(int i=0;i<count;i++)if(!private_variable(environ[i]))env.push_back(environ[i]);
    env.push_back(nullptr);
    FILE *output=tmpfile();
    int nullfd=open("/dev/null",O_RDONLY);
    if(!output||nullfd<0){
        if(output)fclose(output);
        if(nullfd>=0)close(nullfd);
        return err("无法创建命令输出文件。");
    }
    double start=monotime();
    pid_t child=fork();
    if(child<0){
        fclose(output);
        close(nullfd);
        return err("无法启动命令。");
    }
    if(child==0){
        signal(SIGINT,SIG_DFL);
        signal(SIGTERM,SIG_DFL);
        struct rlimit limit={
            2000000,2000000
        };
        if(setpgid(0,0)||chdir(a->root)||setrlimit(RLIMIT_FSIZE,&limit)||dup2(nullfd,0)<0||dup2(fileno(output),1)<0||dup2(fileno(output),2)<0)_exit(126);
        close(nullfd);
        if(fileno(output)>2)close(fileno(output));
        char bash[]="bash",profile[]="--noprofile",rc[]="--norc",option[]="-c";
        char *argv[]={bash,profile,rc,option,const_cast<char *>(command),nullptr};
        execve("/bin/bash",argv,env.data());
        _exit(127);
    }
    close(nullfd);
    if(setpgid(child,child)<0&&errno!=EACCES&&errno!=ESRCH){
        kill(child,SIGKILL);
        while(waitpid(child,NULL,0)<0&&errno==EINTR){
        }
        fclose(output);
        return err("无法建立命令进程组。");
    }
    int status=0;
    bool reaped=false,timed=false,failed=false;
    while(!interrupted){
        pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child){
            reaped=true;
            break;
        }
        if(result<0&&errno!=EINTR){
            failed=true;
            break;
        }
        if((monotime()-start)*1000>=timeout_ms){
            timed=true;
            break;
        }
        struct timespec pause={
            0,1000000
        };
        nanosleep(&pause,NULL);
    }
    kill(-child,SIGKILL);
    if(!reaped){
        pid_t result;
        do{
            result=waitpid(child,&status,0);
        }
        while(result<0&&errno==EINTR);
        if(result!=child)failed=true;
    }
    if(interrupted){
        fclose(output);
        return err("命令已取消。");
    }
    if(failed){
        fclose(output);
        return err("命令等待失败，未自动重新执行。");
    }
    rewind(output);
    char data[16001];
    size_t len=fread(data,1,sizeof data,output);
    bool io_error=ferror(output);
    fclose(output);
    if(io_error)return err("读取命令输出失败。");
    bool truncated=len>16000;
    if(truncated)len=16000;
    std::string safe;
    safe.reserve(len*3);
    for(size_t i=0;i<len;){
        unsigned char c=(unsigned char)data[i];
        size_t width=c<0x80?1:(c>=0xc2&&c<=0xdf)?2:(c>=0xe0&&c<=0xef)?3:(c>=0xf0&&c<=0xf4)?4:0;
        bool valid=width&&i+width<=len;
        for(size_t k=1;valid&&k<width;k++)if(((unsigned char)data[i+k]&0xc0)!=0x80)valid=false;
        if(valid&&width==3&&((c==0xe0&&(unsigned char)data[i+1]<0xa0)||(c==0xed&&(unsigned char)data[i+1]>=0xa0)))valid=false;
        if(valid&&width==4&&((c==0xf0&&(unsigned char)data[i+1]<0x90)||(c==0xf4&&(unsigned char)data[i+1]>=0x90)))valid=false;
        if(valid){
            safe.append(data+i,width);
            i+=width;
        }
        else{
            safe+="\xef\xbf\xbd";
            i++;
        }
    }
    J *r=obj();
    put(r,"exit_code",json_object_new_int(WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status)));
    put(r,"timed_out",json_object_new_boolean(timed));
    put(r,"truncated",json_object_new_boolean(truncated));
    put(r,"output",json_object_new_string_len(safe.data(),static_cast<int>(safe.size())));
    return r;
}
