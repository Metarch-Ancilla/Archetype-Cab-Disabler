/* One-batch macOS authorisation bridge and descriptor-only privileged helper.
 * No Python, commands, ownership changes, file creation or preset parsing as root.
 * The normal-account backend validates and writes through returned descriptors.
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../shared/privilege_io.h"
#include <sys/wait.h>
#include <pthread.h>

#ifdef CAB_PRIVILEGED_HELPER
/* Exact batch allowlist arrives over the private inherited channel. */
static int compare_paths(const void *a,const void *b) {return strcmp(*(const char *const *)a,*(const char *const *)b);}
static int open_preset(const char *path) {
    if(path[0]!='/'||strlen(path)<5||strcasecmp(path+strlen(path)-4,".xml")) {
        errno=EACCES;return -1;
    }
    char copy[PATH_MAX];strcpy(copy,path+1);
    int directory=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory<0)return -1;
    char *save=NULL,*part=strtok_r(copy,"/",&save);
    while(part) {
        char *next=strtok_r(NULL,"/",&save);
        if(!strcmp(part,".")||!strcmp(part,"..")){close(directory);errno=EACCES;return -1;}
        // Resolve each component through the preceding directory, rejecting symlinks.
        int fd=openat(directory,part,next?O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC:O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        int error=errno;close(directory);directory=fd;errno=error;
        if(fd<0)return -1;
        part=next;
    }
    struct stat info;
    if(fstat(directory,&info)||!S_ISREG(info.st_mode)||info.st_size>4*1024*1024) {
        close(directory);errno=EACCES;return -1;
    }return directory;
}
int main(int argc,char **argv) {
    (void)argv;if(argc!=1||geteuid()!=0)return 1;
    if(cab_io_setup(STDIN_FILENO)||cab_io_setup(STDOUT_FILENO))return 1;
    struct CabIO io={STDIN_FILENO,NULL,NULL,30000}, output={STDOUT_FILENO,NULL,NULL,30000};
    struct CabHello hello={CAB_HELLO_MAGIC,1,(uint32_t)getpid(),(uint32_t)getppid()};
    if(cab_transfer(&output,&hello,sizeof(hello),1))return 1;
    uint32_t count=0;size_t total=0;
    if(cab_transfer(&io,&count,sizeof(count),0)||!count||count>50000)return 1;
    char **approved=calloc(count,sizeof(*approved));if(!approved)return 1;
    for(uint32_t i=0;i<count;++i) {
        uint32_t size=0;if(cab_transfer(&io,&size,sizeof(size),0)||!size||size>=PATH_MAX)return 1;
        total+=size+1;if(total>64*1024*1024)return 1;
        approved[i]=malloc(size+1);if(!approved[i])return 1;
        if(cab_transfer(&io,approved[i],size,0)||memchr(approved[i],0,size))return 1;
        approved[i][size]=0;
        if(approved[i][0]!='/'||size<5||strcasecmp(approved[i]+size-4,".xml"))return 1;
    }
    qsort(approved,count,sizeof(*approved),compare_paths);
    int acknowledged=0;if(cab_transfer(&output,&acknowledged,sizeof(acknowledged),1))return 1;
    /* Client EOF ends the batch. No installed daemon or persistent privileged service. */
    for(unsigned requests=0;requests<100000;++requests) {
        uint32_t size=0;
        if(cab_transfer(&io,&size,sizeof(size),0)||!size)break;
        if(size>=PATH_MAX)return 1;
        char path[PATH_MAX];if(cab_transfer(&io,path,size,0)||memchr(path,0,size))return 1;path[size]=0;
        char *key=path;int fd=-1,error=EACCES;
        if(bsearch(&key,approved,count,sizeof(*approved),compare_paths)) {fd=open_preset(path);error=fd<0?errno:0;}
        int failed=cab_send_fd(&output,fd,error);
        if(fd>=0)close(fd);
        if(failed)return 1;
    }
    for(uint32_t i=0;i<count;++i)free(approved[i]);free(approved);return 0;
}
#else
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
/* High Sierra-compatible one-shot API. Deprecated by Apple, still declared by
 * the target SDK. A signed launchd helper is the future stable-release path. */
struct Batch { AuthorizationRef auth; FILE *channel; struct CabIO io; pid_t child; struct CabHello hello; int reap_slot; };
/* Reserve bounded reaper capacity before launching anything privileged. */
static pthread_once_t reaper_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t reaper_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaper_ready=PTHREAD_COND_INITIALIZER;
static pid_t reap_slots[64];
static int reaper_error;
static void *reap_children(void *unused) {
    (void)unused;pthread_mutex_lock(&reaper_mutex);
    for(;;) {
        int active=0;
        for(unsigned i=0;i<64;++i)if(reap_slots[i]>0) {
            pid_t result=waitpid(reap_slots[i],NULL,WNOHANG);
            if(result==reap_slots[i]||(result<0&&errno==ECHILD))reap_slots[i]=0;
            else active=1;
        }
        if(active) {
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=100000000;
            if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
            pthread_cond_timedwait(&reaper_ready,&reaper_mutex,&until);
        } else pthread_cond_wait(&reaper_ready,&reaper_mutex);
    }
    return NULL;
}
static void start_reaper(void) {
    pthread_t thread;reaper_error=pthread_create(&thread,NULL,reap_children,NULL);
    if(!reaper_error)pthread_detach(thread);
}
static int reserve_reaper(void) {
    pthread_once(&reaper_once,start_reaper);
    if(reaper_error){errno=reaper_error;return -1;}
    pthread_mutex_lock(&reaper_mutex);int slot=-1;
    for(unsigned i=0;i<64;++i)if(!reap_slots[i]){reap_slots[i]=-1;slot=(int)i;break;}
    pthread_mutex_unlock(&reaper_mutex);if(slot<0)errno=EAGAIN;return slot;
}
static void close_batch(struct Batch *batch) {
    if(batch->channel){shutdown(fileno(batch->channel),SHUT_RDWR);fclose(batch->channel);}
    if(batch->auth)AuthorizationFree(batch->auth,kAuthorizationFlagDestroyRights);
    pthread_mutex_lock(&reaper_mutex);reap_slots[batch->reap_slot]=batch->child;
    pthread_cond_signal(&reaper_ready);pthread_mutex_unlock(&reaper_mutex);free(batch);
}
void *cab_authorize_v2(const char *helper,const char *const *paths,uint32_t count,int *status,
                    int (*cancel)(void *),void *context) {
    *status=0;
    if(!count||count>50000){*status=EINVAL;return NULL;}
    size_t total=0;
    for(uint32_t i=0;i<count;++i) {
        size_t size=paths[i]?strlen(paths[i]):0;
        if(!size||size>=PATH_MAX||(total+=size+1)>64*1024*1024){*status=EINVAL;return NULL;}
    }
    struct Batch *batch=calloc(1,sizeof(*batch));if(!batch){*status=ENOMEM;return NULL;}
    batch->reap_slot=reserve_reaper();
    if(batch->reap_slot<0){*status=errno;free(batch);return NULL;}
    OSStatus result=AuthorizationCreate(NULL,kAuthorizationEmptyEnvironment,kAuthorizationFlagDefaults,&batch->auth);
    if(result==errAuthorizationSuccess) {
        const char *prompt="Direct edit requires elevated permissions\n\nEnter your password to authorize this action.";
        AuthorizationItem environmentItem={kAuthorizationEnvironmentPrompt,(UInt32)strlen(prompt),(void *)prompt,0};
        AuthorizationEnvironment environment={1,&environmentItem};
        AuthorizationItem item={kAuthorizationRightExecute,0,NULL,0};AuthorizationRights rights={1,&item};
        result=AuthorizationCopyRights(batch->auth,&rights,&environment,
            kAuthorizationFlagInteractionAllowed|kAuthorizationFlagExtendRights|kAuthorizationFlagPreAuthorize,NULL);
    }
    if(result==errAuthorizationSuccess) {
        char *arguments[]={NULL};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        result=AuthorizationExecuteWithPrivileges(batch->auth,helper,kAuthorizationFlagDefaults,arguments,&batch->channel);
#pragma clang diagnostic pop
    }
    if(result!=errAuthorizationSuccess||!batch->channel) {
        *status=result==errAuthorizationCanceled?ECANCELED:EACCES;
        close_batch(batch);return NULL;
    }
    int fd=fileno(batch->channel);
    batch->io=(struct CabIO){fd,cancel,context,30000};
    struct CabHello hello={0};
    if(cab_io_setup(fd)||cab_transfer(&batch->io,&hello,sizeof(hello),0)||
       hello.magic!=CAB_HELLO_MAGIC||hello.version!=1||hello.pid<=1||hello.pid>INT_MAX||hello.pid==(uint32_t)getpid()) {
        *status=errno?errno:EPROTO;close_batch(batch);return NULL;
    }
    batch->hello=hello;
    if(hello.parent==(uint32_t)getpid()&&hello.pid!=(uint32_t)getpid()) {
        // waitpid verifies kernel parentage; reported PIDs alone are not trusted.
        pid_t child=(pid_t)hello.pid;pid_t result;
        do{result=waitpid(child,NULL,WNOHANG);}while(result<0&&errno==EINTR);
        if(result==0)batch->child=child;
    }
    int failed=cab_transfer(&batch->io,&count,sizeof(count),1);
    for(uint32_t i=0;i<count&&!failed;++i) {
        uint32_t size=(uint32_t)strlen(paths[i]);
        failed=cab_transfer(&batch->io,&size,sizeof(size),1)||cab_transfer(&batch->io,(void *)paths[i],size,1);
    }
    int acknowledged=EIO;
    if(failed||cab_transfer(&batch->io,&acknowledged,sizeof(acknowledged),0)||acknowledged) {
        *status=errno?errno:EIO;close_batch(batch);return NULL;
    }
    return batch;
}
int cab_authorized_open(void *handle,const char *path) {
    struct Batch *batch=handle;size_t length=strlen(path);
    if(!length||length>=PATH_MAX){errno=EINVAL;return -1;}
    uint32_t size=(uint32_t)length;int socket=fileno(batch->channel);
    (void)socket;
    if(cab_transfer(&batch->io,&size,sizeof(size),1)||cab_transfer(&batch->io,(void *)path,length,1))return -1;
    return cab_receive_fd(&batch->io);
}
int cab_authorization_info(void *handle,uint32_t *pid,uint32_t *parent,int *child) {
    struct Batch *batch=handle;if(!batch||!pid||!parent||!child)return -1;
    *pid=batch->hello.pid;*parent=batch->hello.parent;*child=batch->child>0;return 0;
}
void cab_authorization_close(void *handle) {
    struct Batch *batch=handle;if(!batch)return;
    // Shutdown cannot wait for a stalled peer to read a stop message.
    close_batch(batch);
}

#endif
