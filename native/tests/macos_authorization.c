/* Explicit opt-in native authorization probe. Only its own scratch file is writable. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
void *cab_authorize_v2(const char *,const char *const *,uint32_t,int *,int (*)(void *),void *);
int cab_authorized_open(void *,const char *);
void cab_authorization_close(void *);
int cab_authorization_info(void *,uint32_t *,uint32_t *,int *);
int main(int argc,char **argv) {
    if(argc!=4||strcmp(argv[1],"--authorize")) {
        fprintf(stderr,"Usage: CabAuthorizationProbe --authorize /path/to/CabPrivilege /absolute/scratch/folder\nThis displays an administrator prompt and edits ONLY a generated scratch file.\n");return 2;
    }
    if(argv[3][0]!='/'){fprintf(stderr,"Scratch folder must be absolute\n");return 2;}
    char folder[4096];if(snprintf(folder,sizeof(folder),"%s/cab-probe-XXXXXX",argv[3])>=(int)sizeof(folder)||!mkdtemp(folder)){perror("scratch");return 1;}
    char path[4096];if(snprintf(path,sizeof(path),"%s/Fixture.xml",folder)>=(int)sizeof(path))return 1;
    int source=open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);if(source<0){perror("fixture");return 1;}
    const char original[]="<probe cabActive=\"1\"/>";
    if(write(source,original,sizeof(original)-1)!=sizeof(original)-1){close(source);return 1;}
    struct stat before,after;if(fstat(source,&before)){close(source);return 1;}
    const char *paths[]={path};int status=0;void *batch=cab_authorize_v2(argv[2],paths,1,&status,NULL,NULL);
    if(!batch){fprintf(stderr,"Authorization failed/cancelled: %d\n",status);close(source);unlink(path);rmdir(folder);return 1;}
    uint32_t pid=0,parent=0;int child=0;
    if(cab_authorization_info(batch,&pid,&parent,&child)){cab_authorization_close(batch);close(source);return 1;}
    int fd=cab_authorized_open(batch,path),valid=fd>=0&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino;
    if(valid) {char changed[]="<probe cabActive=\"0\"/>";valid=pwrite(fd,changed,sizeof(changed)-1,0)==sizeof(changed)-1&&!fsync(fd);}
    if(fd>=0)close(fd);
    errno=0;int rejected=cab_authorized_open(batch,"/tmp/CabNotApproved.xml");int denied=rejected<0;if(rejected>=0)close(rejected);
    cab_authorization_close(batch);
    int reaped=0;
    if(child)for(int i=0;i<100;++i){siginfo_t info;memset(&info,0,sizeof(info));errno=0;int result=waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT);if(result<0&&errno==ECHILD){reaped=1;break;}struct timespec pause={0,20000000};nanosleep(&pause,NULL);}
    char data[sizeof(original)]={0};valid=valid&&pread(source,data,sizeof(original)-1,0)==sizeof(original)-1&&strstr(data,"\"0\"");close(source);
    printf("{\"descriptor_identity_and_scratch_write\":%s,\"unapproved_path_rejected\":%s,\"helper_pid\":%u,\"helper_parent\":%u,\"backend_pid\":%u,\"verified_direct_child\":%s,\"reaped_by_bridge\":%s}\n",valid?"true":"false",denied?"true":"false",pid,parent,(unsigned)getpid(),child?"true":"false",child?(reaped==1?"true":"false"):"null");
    unlink(path);rmdir(folder);return valid&&denied&&(!child||reaped==1)?0:1;
}
