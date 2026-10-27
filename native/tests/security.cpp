// Scratch-file diagnostics: no supplied presets, elevation or application installation.
#include "../backend/core.hpp"
#include "../../shared/channel.hpp"
#include "../../frontends/shared/wire.hpp"
#include <iostream>
#ifndef _WIN32
#include "../../frontends/shared/privilege_io.h"
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#ifdef CAB_SECURITY_PRIVILEGED_TEST
#include <shellapi.h>
#include "../../frontends/windows/authorization.h"
#endif
#endif
using namespace cab;
static void rejects(const std::function<void()> &work) {
    bool failed=false;try{work();}catch(const std::exception &){failed=true;}
    require(failed,"Security boundary accepted substituted input");
}
static void put(const Path &path,const Bytes &data) {
    auto file=createExclusive(path);writeAll(file.handle,data);
}
#ifndef _WIN32
static void descriptorTests() {
    int sockets[2];require(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets),"socketpair failed");
    File a(sockets[0]),b(sockets[1]);require(!cab_io_setup(sockets[0])&&!cab_io_setup(sockets[1]),"I/O setup failed");
    CabIO read{sockets[0],nullptr,nullptr,40},write{sockets[1],nullptr,nullptr,40};
    int value=0;auto start=cab_now();
    require(cab_transfer(&read,&value,sizeof(value),0)<0&&errno==ETIMEDOUT&&cab_now()-start<1000,"Stalled peer deadline failed");
    read.cancel=[](void *){return 1;};
    require(cab_transfer(&read,&value,sizeof(value),0)<0&&errno==ECANCELED,"Cancellation failed");read.cancel=nullptr;
    File file(::open("/dev/null",O_RDONLY));require(file.handle>=0,"Cannot open descriptor fixture");
    require(!cab_send_fd(&write,(int)file.handle,0),"Descriptor send failed");
    File received(cab_receive_fd(&read));require(received.handle>=0&&(fcntl((int)received.handle,F_GETFD)&FD_CLOEXEC),"Descriptor transfer failed");
    auto countFds=[] { int count=0;for(int fd=0;fd<256;++fd)if(fcntl(fd,F_GETFD)>=0)++count;return count; };
    int baseline=countFds();
    int error=0;struct iovec iov{&error,sizeof(error)};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(2*sizeof(int))];} control{};
    struct msghdr message{};message.msg_iov=&iov;message.msg_iovlen=1;message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    auto header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(2*sizeof(int));
    int duplicated[]={(int)file.handle,(int)file.handle};std::memcpy(CMSG_DATA(header),duplicated,sizeof(duplicated));
    require(sendmsg(sockets[1],&message,0)==sizeof(error),"Malformed descriptor fixture failed");
    require(cab_receive_fd(&read)<0&&errno==EPROTO&&countFds()==baseline,"Duplicate descriptors accepted or leaked");
    iov.iov_len=2;header->cmsg_len=CMSG_LEN(sizeof(int));message.msg_controllen=CMSG_SPACE(sizeof(int));
    require(sendmsg(sockets[1],&message,0)==2,"Partial reply fixture failed");
    require(cab_receive_fd(&read)<0&&errno==ETIMEDOUT&&countFds()==baseline,"Partial descriptor reply did not expire cleanly");
    std::cout<<"descriptor_timeout_cancel_cloexec_duplicate_rejection: passed\n";
}
#endif
int main(int argc,char **argv) {
    try {
#ifdef CAB_SECURITY_PRIVILEGED_TEST
        int wideCount=0;wchar_t **wideArgs=CommandLineToArgvW(GetCommandLineW(),&wideCount);
        if(!wideArgs)return 1;
        if(wideCount>1&&!wcscmp(wideArgs[1],L"--privileged-helper")) {
            int result=cab_privileged_main(wideCount,wideArgs);LocalFree(wideArgs);return result;
        }
        LocalFree(wideArgs);
        bool authorize=argc==3&&std::string(argv[2])=="--authorize";
        if(authorize)--argc;
#endif
        std::cout<<"source_version: "<<Version<<'\n';
        require(argc==2,"Provide an empty scratch directory");Path base=absolute(argv[1]);mkdirs(base);
        nd::ByteQueue queue(8);std::string value;
        require(queue.push("12345678",false)&&!queue.push("x",false),"Queue byte limit failed");
        require(queue.push("cancel",false,true)&&queue.pop(value,false)&&value=="cancel","Urgent command did not replace backlog");
        queue.close();require(!queue.push("x",false)&&!queue.pop(value,false),"Closed queue remained writable");
        nd::Model model;model.record({"STATE","0","0","0","1","4e657874","54","","","0","0"});
        model.record({"CONTROL","presets","tree","400","",""});
        for(size_t i=0;i<100001;++i)model.record({"OPTION","presets","30","54","0","-1","preset","1",""});
        rejects([&]{model.record({"OPTION","presets","30","54","0","-1","preset","1",""});});
        std::cout<<"bounded_queue_and_maximum_model: passed\n";
        nd::CommandWriter writer;std::atomic<bool> writing{false};
        writer.start([&](const std::string &) { writing=true;while(!writer.stopping())std::this_thread::yield();return false; });
        require(writer.post("queued"),"Async writer rejected first command");
        while(!writing)std::this_thread::yield();
        require(writer.post("cancel",true),"UI command blocked behind writer");writer.close();writer.interrupt();writer.join();
#ifdef _WIN32
        HANDLE pipeRead=nullptr,pipeWrite=nullptr;require(CreatePipe(&pipeRead,&pipeWrite,nullptr,1024),"Cannot create stalled-peer fixture");
        File reading((Handle)pipeRead),sending((Handle)pipeWrite);nd::CommandWriter stalled;
        std::atomic<bool> entered{false};stalled.start([&](const std::string &bytes){entered=true;DWORD sent=0;return WriteFile(pipeWrite,bytes.data(),(DWORD)bytes.size(),&sent,nullptr)&&sent==bytes.size();});
        require(stalled.post(Bytes(256*1024,'x')),"Cannot queue stalled write");while(!entered)std::this_thread::yield();
        auto start=std::chrono::steady_clock::now();stalled.close();stalled.interrupt();stalled.join();
        require(std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"Stalled Windows writer did not close promptly");
        std::cout<<"stalled_windows_pipe_shutdown: passed\n";
#endif
        Path source=join(base,"BackupSource.xml"), archive=join(base,"Race.zip");put(source,"<p cabActive=\"1\"/>");
        std::atomic<bool> cancel{false};auto plan=preview({{source,base,"Fixture","Factory"}},"disable",cancel);
        auto results=apply(plan,archive,cancel,{},[&](const std::string &){if(!exists(archive))put(archive,"replacement zip");});
        require(results[0].status=="failed"&&readSnapshot(archive).data=="replacement zip"&&
            readSnapshot(source).data=="<p cabActive=\"1\"/>","Backup collision was deleted or edits began");
        Path directory=join(base,"Destination");mkdirs(directory);
        Path target=join(directory,"Tone.xml");
        publishCopy(target,"verified",[]{});
        rejects([&]{publishCopy(target,"overwrite",[]{});});
        require(readSnapshot(target).data=="verified","Existing destination overwritten");
        Path replaced=join(directory,"Race.xml");
        rejects([&]{publishCopy(replaced,"verified",[&]{put(replaced,"replacement");});});
        require(readSnapshot(replaced).data=="replacement","Replacement deleted during cleanup");
#ifndef _WIN32
        Path trap=join(base,"Trap");mkdirs(trap);
        rejects([&]{publishCopy(join(directory,"Ancestor.xml"),"verified",[&]{
            require(!rename(directory.c_str(),(directory+"-held").c_str()),"Rename fixture failed");
            require(!symlink(trap.c_str(),directory.c_str()),"Symlink fixture failed");
        });});
        require(!exists(join(trap,"Ancestor.xml")),"Directory substitution escaped anchor");
        descriptorTests();
#endif
#ifdef CAB_SECURITY_PRIVILEGED_TEST
        if(authorize) {
            auto before=readSnapshot(source);Authorization batch;batch.authorize({source});
            File allowed(batch.open(source));require(signature(allowed.handle)==before.stamp,"Elevated broker opened wrong fixture");
            rejects([&]{File rejected(batch.open(target));});
            auto changed=patch(before.data,false);seek(allowed.handle,0);writeAll(allowed.handle,changed);
            truncate(allowed.handle,changed.size());sync(allowed.handle);seek(allowed.handle,0);
            require(readHandle(allowed.handle)==changed,"Elevated fixture write failed");
            std::cout<<"explicit_uac_allowlist_identity_and_scratch_write: passed\n";
        }
#endif
        std::cout<<"no_clobber_staging_and_replacement_cleanup: passed\n";
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
