#pragma once
/* Bounded descriptor protocol, also exercised by the unprivileged diagnostic harness. */
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
struct CabIO { int fd; int (*cancel)(void *); void *context; unsigned timeout_ms; };
struct CabHello { uint32_t magic, version, pid, parent; };
#define CAB_HELLO_MAGIC 0x43414232u
static inline int64_t cab_now(void) {
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time))return -1;
    return (int64_t)time.tv_sec*1000+time.tv_nsec/1000000;
}
static inline int cab_io_setup(int fd) {
    int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK))return -1;
    flags=fcntl(fd,F_GETFD);if(flags<0||fcntl(fd,F_SETFD,flags|FD_CLOEXEC))return -1;
#ifdef __APPLE__
    int one=1;if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one)))return -1;
#endif
    int type=0;socklen_t size=sizeof(type);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_STREAM){errno=EPROTOTYPE;return -1;}
    return 0;
}
static inline int cab_wait(struct CabIO *io,short events,int64_t deadline) {
    for(;;) {
        if(io->cancel&&io->cancel(io->context)){errno=ECANCELED;return -1;}
        int64_t now=cab_now();if(now<0)return -1;
        if(now>=deadline){errno=ETIMEDOUT;return -1;}
        int timeout=(int)(deadline-now);if(timeout>100)timeout=100;
        struct pollfd poller={io->fd,events,0};int result=poll(&poller,1,timeout);
        if(result<0&&errno==EINTR)continue;
        if(result<0)return -1;
        if(result&&poller.revents&(events|POLLHUP|POLLERR))return 0;
        if(result&&poller.revents&POLLNVAL){errno=EBADF;return -1;}
    }
}
static inline int cab_transfer_until(struct CabIO *io,void *bytes,size_t size,int sending,int64_t deadline) {
    char *at=(char *)bytes;
    while(size) {
        if(cab_wait(io,sending?POLLOUT:POLLIN,deadline))return -1;
#ifdef MSG_NOSIGNAL
        ssize_t n=sending?send(io->fd,at,size,MSG_NOSIGNAL):read(io->fd,at,size);
#else
        ssize_t n=sending?send(io->fd,at,size,0):read(io->fd,at,size);
#endif
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0){if(!n)errno=EPIPE;return -1;}
        at+=n;size-=(size_t)n;
    }
    return 0;
}
static inline int cab_transfer(struct CabIO *io,void *bytes,size_t size,int sending) {
    int64_t now=cab_now();if(now<0)return -1;
    return cab_transfer_until(io,bytes,size,sending,now+io->timeout_ms);
}
static inline int cab_receive_fd(struct CabIO *io) {
    int error=EIO,received=-1;size_t offset=0;unsigned descriptors=0;int invalid=0;
    int64_t now=cab_now();if(now<0)return -1;int64_t deadline=now+io->timeout_ms;
    while(offset<sizeof(error)) {
        if(cab_wait(io,POLLIN,deadline)){invalid=errno;break;}
        union {struct cmsghdr align;char bytes[CMSG_SPACE(8*sizeof(int))];} control;
        memset(&control,0,sizeof(control));
        struct iovec iov={(char *)&error+offset,sizeof(error)-offset};
        struct msghdr msg;memset(&msg,0,sizeof(msg));msg.msg_iov=&iov;msg.msg_iovlen=1;
        msg.msg_control=control.bytes;msg.msg_controllen=sizeof(control.bytes);
        ssize_t n=recvmsg(io->fd,&msg,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0){invalid=n?errno:EPIPE;break;}
        for(struct cmsghdr *h=CMSG_FIRSTHDR(&msg);h;h=CMSG_NXTHDR(&msg,h)) {
            if(h->cmsg_level!=SOL_SOCKET||h->cmsg_type!=SCM_RIGHTS||h->cmsg_len<CMSG_LEN(0)) {invalid=EPROTO;continue;}
            size_t bytes=h->cmsg_len-CMSG_LEN(0);
            if(bytes%sizeof(int))invalid=EPROTO;
            for(size_t i=0;i+sizeof(int)<=bytes;i+=sizeof(int)) {
                int fd;memcpy(&fd,(char *)CMSG_DATA(h)+i,sizeof(fd));++descriptors;
                if(received<0)received=fd;else close(fd);
            }
        }
        if(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))invalid=EPROTO;
        offset+=(size_t)n;
    }
    if(invalid||offset!=sizeof(error)||error||descriptors!=1) {
        if(received>=0)close(received);errno=invalid?invalid:error?error:EPROTO;return -1;
    }
    int flags=fcntl(received,F_GETFD);
    if(flags<0||fcntl(received,F_SETFD,flags|FD_CLOEXEC)){int saved=errno;close(received);errno=saved;return -1;}
    return received;
}
static inline int cab_send_fd(struct CabIO *io,int fd,int error) {
    struct iovec iov={&error,sizeof(error)};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control;
    struct msghdr msg;memset(&msg,0,sizeof(msg));msg.msg_iov=&iov;msg.msg_iovlen=1;
    if(fd>=0) {
        memset(&control,0,sizeof(control));msg.msg_control=control.bytes;msg.msg_controllen=sizeof(control.bytes);
        struct cmsghdr *h=CMSG_FIRSTHDR(&msg);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h),&fd,sizeof(fd));
    }
    int64_t now=cab_now();if(now<0)return -1;int64_t deadline=now+io->timeout_ms;
    for(;;) {
        if(cab_wait(io,POLLOUT,deadline))return -1;
#ifdef MSG_NOSIGNAL
        ssize_t n=sendmsg(io->fd,&msg,MSG_NOSIGNAL);
#else
        ssize_t n=sendmsg(io->fd,&msg,0);
#endif
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0){if(!n)errno=EPIPE;return -1;}
        return cab_transfer_until(io,(char *)&error+n,sizeof(error)-(size_t)n,1,deadline);
    }
}
