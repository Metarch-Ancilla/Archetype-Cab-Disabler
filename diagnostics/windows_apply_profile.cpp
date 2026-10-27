// Developer diagnostic only. Original presets are read; all edits target disposable copies.
#include "core.hpp"
#include "windows_apply_profile.hpp"
#include "authorization.h"
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <iostream>
#include <sstream>

extern "C" void profile_shell(int begin) {
    static std::optional<prof::Scope> scope;
    if (begin) scope.emplace(prof::ShellLaunch); else scope.reset();
}
static std::string utf8(const wchar_t *value) {
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,nullptr,0,nullptr,nullptr);
    cab::require(size>0,"Invalid Unicode argument");std::string out(size,0);
    WideCharToMultiByte(CP_UTF8,0,value,-1,out.data(),size,nullptr,nullptr);out.pop_back();return out;
}
static bool wine() {return GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")!=nullptr;}
static bool elevated() {
    HANDLE token;cab::require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"Cannot inspect token");
    TOKEN_ELEVATION level={};DWORD bytes=0;BOOL valid=GetTokenInformation(token,TokenElevation,&level,sizeof(level),&bytes);
    CloseHandle(token);cab::require(valid,"Cannot inspect elevation");return level.TokenIsElevated;
}
static void protectCopies(const std::vector<cab::Source> &sources,const cab::Path &root) {
    HANDLE token;cab::require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"Cannot inspect user token");
    DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);std::vector<unsigned char> user(bytes);
    BOOL valid=GetTokenInformation(token,TokenUser,user.data(),bytes,&bytes);CloseHandle(token);
    cab::require(valid,"Cannot inspect user SID");LPWSTR sid=nullptr;
    cab::require(ConvertSidToStringSidW(((TOKEN_USER *)user.data())->User.Sid,&sid),"Cannot encode user SID");
    std::wstring sddl=L"D:P(A;;FRSD;;;"+std::wstring(sid)+L")(A;;FA;;;BA)(A;;FA;;;SY)";LocalFree(sid);
    PSECURITY_DESCRIPTOR raw=nullptr;
    cab::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"Cannot construct fixture ACL");
    struct Descriptor {PSECURITY_DESCRIPTOR value;~Descriptor(){LocalFree(value);}} descriptor{raw};
    PACL acl=nullptr;BOOL present=FALSE,defaults=FALSE;
    cab::require(GetSecurityDescriptorDacl(raw,&present,&acl,&defaults)&&present,"Missing fixture ACL");
    for(const auto &source:sources) {
        cab::require(!cab::relative(source.path,root).empty(),"Refusing to change source permissions");
        cab::safePath(source.path);
        int size=MultiByteToWideChar(CP_UTF8,0,source.path.data(),(int)source.path.size(),nullptr,0);
        std::wstring path(size,0);MultiByteToWideChar(CP_UTF8,0,source.path.data(),(int)source.path.size(),path.data(),size);
        cab::require(SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
                                         nullptr,nullptr,acl,nullptr)==ERROR_SUCCESS,"Cannot protect disposable fixture");
    }
}
static double cpu() {
    FILETIME a,b,k,u;cab::require(GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u),"CPU clock failed");
    return (((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime)/1e7+
           (((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime)/1e7;
}
static std::string filesystem(const cab::Path &path) {
    int size=MultiByteToWideChar(CP_UTF8,0,path.data(),(int)path.size(),nullptr,0);
    std::wstring wide(size,0);MultiByteToWideChar(CP_UTF8,0,path.data(),(int)path.size(),wide.data(),size);
    wchar_t volume[32768],name[64];
    if(GetVolumePathNameW(wide.c_str(),volume,32768)&&
       GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,name,64))return utf8(name);
    return "unknown";
}
static void cleanup(const cab::Path &root) {
    for(auto &entry:cab::entries(root)) {
        if(entry.link)continue;
        if(entry.dir)cleanup(entry.path);else cab::removeFile(entry.path);
    }
    auto wide=std::wstring();int size=MultiByteToWideChar(CP_UTF8,0,root.data(),(int)root.size(),nullptr,0);
    wide.resize(size);MultiByteToWideChar(CP_UTF8,0,root.data(),(int)root.size(),wide.data(),size);
    RemoveDirectoryW(wide.c_str());
}
static void licenses() {
    HMODULE module=GetModuleHandleW(nullptr);HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(102),RT_RCDATA);
    HGLOBAL data=resource?LoadResource(module,resource):nullptr;DWORD size=resource?SizeofResource(module,resource):0;
    cab::require(data&&size,"Missing license resource");std::cout.write((const char *)LockResource(data),size);
}
struct PhaseResult {std::string json;bool success;};
static PhaseResult measure(const std::vector<cab::Source> &sources,const cab::Path &root,bool protectedMode,std::atomic<bool> &cancel) {
    using namespace cab;
    auto reset=preview(sources,"enable",cancel);auto prepared=apply(reset,"",cancel);
    for(auto &item:prepared)require(item.status=="changed"||item.status=="already-correct","Cannot reset temporary fixtures");
    if(protectedMode&&!wine())protectCopies(sources,root);
    auto plan=preview(sources,"disable",cancel,"",protectedMode);for(auto &item:plan.items)require(item.status=="ready","Fixture preview failed");
    std::cerr<<"Testing "<<(protectedMode?"protected":"ordinary")<<" Direct Edit on "<<sources.size()<<" copies, without backup.\n";
    auto start=std::chrono::steady_clock::now();double cpuStart=cpu();prof::stats={};prof::pathComponents=prof::readBytes=prof::writeBytes=0;prof::forceBroker=protectedMode&&wine();prof::enabled=true;
    auto result=apply(plan,"",cancel);prof::enabled=false;prof::forceBroker=false;
    double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();double used=cpu()-cpuStart;
    size_t changed=0;for(auto &item:result)if(item.status=="changed")++changed;
    std::ostringstream json;
    json<<"{\"mode\":\""<<(protectedMode?"protected":"ordinary")<<"_direct_edit_no_backup\","
        <<"\"protected_access\":\""<<(protectedMode?(wine()?"simulated_for_wine":"real_acl"):"none")<<"\","
        <<"\"tested_files\":"<<sources.size()<<",\"changed_files\":"<<changed
        <<",\"wall_seconds_including_uac\":"<<wall<<",\"backend_cpu_seconds\":"<<used
        <<",\"seconds_excluding_shell_execute_and_uac\":"<<wall-prof::stats[prof::ShellLaunch].ns/1e9
        <<",\"path_components\":"<<prof::pathComponents<<",\"read_bytes\":"<<prof::readBytes
        <<",\"write_bytes\":"<<prof::writeBytes<<",\n\"stages\":{\n";
    for(size_t i=0;i<prof::Count;++i){auto &x=prof::stats[i];if(i)json<<",\n";
        json<<'"'<<prof::names[i]<<"\":{\"calls\":"<<x.calls<<",\"inclusive_ms\":"<<x.ns/1e6<<",\"exclusive_ms\":"<<x.own/1e6<<'}';}
    json<<"\n}}";return {json.str(),changed==sources.size()};
}
static int run(const cab::Path &input) {
    using namespace cab;
    require(wine()||!elevated(),"Run normally, not as administrator; the protected phase requests UAC itself");
    std::atomic<bool> cancel{false};auto catalog=discover({{absolute(input),false}},cancel);
    wchar_t temporary[32768];DWORD length=GetTempPathW(32768,temporary);
    require(length&&length<32768,"Cannot locate temporary directory");
    auto root=join(absolute(utf8(temporary)),"cab-profile-"+std::to_string(entropy()));
    require(!exists(root),"Temporary path collision");mkdirs(root);
    struct Scratch {Path root;~Scratch(){try{cleanup(root);}catch(...){}}} scratch{root};
    std::vector<Source> sources;size_t copied=0;
    std::cerr<<"Preparing temporary copies. Original presets will not be edited.\n";
    for(size_t index=0;index<catalog.libraries.size();++index) {
        const auto &library=catalog.libraries[index];auto folder=join(root,"Plugin"+std::to_string(index));
        for(const auto &source:library.sources) {
            if(source.category=="User")continue;
            auto snapshot=readSnapshot(source.path);auto target=join(folder,relative(source.path,library.path));
            mkdirs(parent(target));auto file=createExclusive(target);writeAll(file.handle,snapshot.data);sync(file.handle);
            sources.push_back({target,folder,source.product,source.category});++copied;
        }
    }
    require(!sources.empty(),"No factory presets found in the supplied folder");
    auto reset=preview(sources,"enable",cancel);auto resetResult=apply(reset,"",cancel);
    std::vector<Source> supported;
    for(size_t i=0;i<reset.items.size();++i) {
        if(reset.items[i].status=="ready")require(resetResult[i].status=="changed","Could not prepare temporary fixture");
        if(resetResult[i].status=="changed"||resetResult[i].status=="already-correct")supported.push_back(reset.items[i].source);
    }
    require(!supported.empty(),"No supported factory presets found");
    auto ordinary=measure(supported,root,false,cancel);
    std::cerr<<"The next test requests UAC to exercise the protected-file route.\n";
    auto protectedRun=measure(supported,root,true,cancel);
    std::ostringstream json;
    json<<"{\n\"source_commit\":\""<<CAB_PROFILE_COMMIT<<"\",\"source_version\":\""<<Version<<"\","
        <<"\"environment\":\""<<(wine()?"Wine":"Windows")<<"\","
        <<"\"source_filesystem\":"<<cab::json(filesystem(absolute(input)))<<",\"temporary_filesystem\":"<<cab::json(filesystem(root))<<','
        <<"\"copied_files\":"<<copied<<",\"runs\":["<<ordinary.json<<','<<protectedRun.json<<"]}\n";
    std::cout<<json.str();
    Path report=join(desktop(),"windows_apply_profile.json");
    for(int i=2;exists(report);++i)report=join(desktop(),"windows_apply_profile_"+std::to_string(i)+".json");
    auto output=createExclusive(report);writeAll(output.handle,json.str());sync(output.handle);
    std::cerr<<"Report saved on Desktop as "<<filename(report)<<". No original preset paths or contents are recorded.\n";
    require(ordinary.success&&protectedRun.success,"Some diagnostic writes failed; see changed_files in the report");return 0;
}
int wmain(int argc,wchar_t **argv) {
    if(argc>1&&wcscmp(argv[1],L"--privileged-helper")==0)return cab_privileged_main(argc,argv);
    try {
        if(argc==2&&wcscmp(argv[1],L"--licenses")==0){licenses();return 0;}
        if(argc==2&&wcscmp(argv[1],L"--help")==0){std::cout<<"WindowsApplyProfiler.exe [preset catalog folder]\nUses disposable copies; originals are never edited. --licenses prints notices.\n";return 0;}
        cab::require(argc<=2,"Provide one preset catalog folder");auto roots=cab::defaultRoots();
        cab::require(argc==2||!roots.empty(),"Provide a preset catalog folder");
        return run(argc==2?utf8(argv[1]):roots[0].path);
    }catch(const std::exception &e){prof::enabled=false;std::cerr<<"Diagnostic failed: "<<e.what()<<'\n';return 1;}
}
