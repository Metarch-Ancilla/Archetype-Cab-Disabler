#pragma once
#include <array>
#include <chrono>
#include <cstdint>
namespace prof {
enum Stage {Apply,Preflight,Direct,PathCheck,OpenRead,OpenWrite,Metadata,Signature,Read,Snapshot,Write,Truncate,Flush,Seek,Hash,Close,Authorization,AuthorizedOpen,FileType,FileInfo,ShellLaunch,AuthorizationClose,Count};
inline const char *names[]={"apply","permission_probe","direct_write","safe_path","open_read","open_write","path_identity","handle_identity","read","snapshot","write","truncate","flush","seek","sha256","close","authorization","authorized_open","file_type","file_info","shell_execute_including_prompt","authorization_close"};
struct Record {uint64_t calls=0,ns=0,own=0;};
inline std::array<Record,Count> stats{};inline bool enabled=false;
inline bool forceBroker=false;
inline uint64_t pathComponents=0,readBytes=0,writeBytes=0;
struct Scope;inline thread_local Scope *top=nullptr;
struct Scope {using Clock=std::chrono::steady_clock;Stage stage;bool active;Scope *parent;Clock::time_point start;uint64_t child=0;
 explicit Scope(Stage s):stage(s),active(enabled),parent(top){if(active){start=Clock::now();top=this;}}
 ~Scope(){if(!active)return;auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();auto &r=stats[stage];++r.calls;r.ns+=ns;r.own+=ns-child;if(parent)parent->child+=ns;top=parent;}
};
}
