/* One-batch UAC descriptor broker. The elevated tool never writes preset bytes. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <errno.h>
#include "authorization.h"
#include "../shared/windows_files.h"

#ifdef CAB_WINDOWS_STATIC_AUTH
#define CAB_AUTH_API
#else
#define CAB_AUTH_API __declspec(dllexport)
#endif

#define PIPE_PREFIX L"\\\\.\\pipe\\ArchetypeCabDisabler-"
#define MAX_PATH_CHARS 32768
#define MAX_FILES 50000
#define IO_TIMEOUT 30000
struct Reply { DWORD error; uint64_t handle; };
static BOOL transfer(HANDLE pipe,void *bytes,DWORD size,BOOL sending) {
    char *p=bytes;
    while(size) {
        OVERLAPPED operation={0};operation.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!operation.hEvent)return FALSE;
        DWORD done=0;BOOL ok=sending?WriteFile(pipe,p,size,&done,&operation):ReadFile(pipe,p,size,&done,&operation);
        if(!ok&&GetLastError()==ERROR_IO_PENDING) {
            if(WaitForSingleObject(operation.hEvent,IO_TIMEOUT)==WAIT_OBJECT_0)ok=GetOverlappedResult(pipe,&operation,&done,FALSE);
            else {CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&done,TRUE);SetLastError(ERROR_TIMEOUT);ok=FALSE;}
        }
        DWORD error=GetLastError();CloseHandle(operation.hEvent);
        if(!ok||!done){SetLastError(ok?ERROR_BROKEN_PIPE:error);return FALSE;}
        p+=done;size-=done;
    }return TRUE;
}
#ifdef CAB_PRIVILEGED_HELPER
static wchar_t *read_path(HANDLE pipe) {
    DWORD length=0;
    if(!transfer(pipe,&length,sizeof(length),FALSE)||!length||length>=MAX_PATH_CHARS)return NULL;
    wchar_t *path=calloc((size_t)length+1,sizeof(wchar_t));if(!path)return NULL;
    if(!transfer(pipe,path,length*sizeof(wchar_t),FALSE)||wmemchr(path,0,length)){free(path);return NULL;}
    return path;
}
static int compare_paths(const void *a,const void *b) {return _wcsicmp(*(const wchar_t *const *)a,*(const wchar_t *const *)b);}
#endif
static BOOL absolute_xml(const wchar_t *path) {
    size_t size=wcslen(path);
    if(size<6||_wcsicmp(path+size-4,L".xml"))return FALSE;
    return (size>3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||(path[0]==L'\\'&&path[1]==L'\\');
}
#ifdef CAB_PRIVILEGED_HELPER
static HANDLE open_preset(const wchar_t *path) {
    if(!absolute_xml(path)){SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    wchar_t *parent=_wcsdup(path);if(!parent){SetLastError(ERROR_OUTOFMEMORY);return INVALID_HANDLE_VALUE;}
    for(wchar_t *p=parent;*p;++p)if(*p==L'/')*p=L'\\';
    wchar_t *slash=wcsrchr(parent,L'\\');
    if(!slash){free(parent);SetLastError(ERROR_INVALID_NAME);return INVALID_HANDLE_VALUE;}
    wchar_t *leaf=_wcsdup(slash+1);if(!leaf){free(parent);SetLastError(ERROR_OUTOFMEMORY);return INVALID_HANDLE_VALUE;}
    if(slash==parent+2)slash[1]=0;else *slash=0;
    HANDLE directory=cab_open_directory(parent,0);free(parent);
    if(directory==INVALID_HANDLE_VALUE){free(leaf);return INVALID_HANDLE_VALUE;}
    HANDLE file=cab_open_child(directory,leaf,GENERIC_READ|GENERIC_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
    DWORD error=GetLastError();CloseHandle(directory);free(leaf);SetLastError(error);
    if(file==INVALID_HANDLE_VALUE)return file;
    BY_HANDLE_FILE_INFORMATION info;LARGE_INTEGER size;
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||!GetFileSizeEx(file,&size)||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||size.QuadPart>4*1024*1024) {
        CloseHandle(file);SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;
    }
    return file;
}
static BOOL elevated(void) {
    HANDLE token=NULL;TOKEN_ELEVATION level;DWORD size=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return FALSE;
    BOOL ok=GetTokenInformation(token,TokenElevation,&level,sizeof(level),&size)&&level.TokenIsElevated;
    CloseHandle(token);return ok;
}
int cab_privileged_main(int argc,wchar_t **argv) {
    if(!argv||argc!=6||wcscmp(argv[1],L"--privileged-helper")||wcscmp(argv[2],L"--pipe")||
       wcscmp(argv[4],L"--parent")||wcsncmp(argv[3],PIPE_PREFIX,wcslen(PIPE_PREFIX))||!elevated())return 1;
    if(!argv[5][0])return 1;
    for(const wchar_t *digit=argv[5];*digit;++digit)if(*digit<L'0'||*digit>L'9')return 1;
    wchar_t *end=NULL;errno=0;DWORD parent=(DWORD)wcstoul(argv[5],&end,10);
    if(!parent||!end||*end||errno==ERANGE)return 1;
    HANDLE pipe=CreateFileW(argv[3],GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    if(pipe==INVALID_HANDLE_VALUE)return 1;
    ULONG server=0;
    // Authenticate the pipe peer before accepting its batch allowlist.
    if(!GetNamedPipeServerProcessId(pipe,&server)||server!=parent){CloseHandle(pipe);return 1;}
    HANDLE client=OpenProcess(PROCESS_DUP_HANDLE,FALSE,parent);
    if(!client){CloseHandle(pipe);return 1;}
    DWORD count=0;wchar_t **approved=NULL;int result=1;size_t total=0;
    if(!transfer(pipe,&count,sizeof(count),FALSE)||!count||count>MAX_FILES)goto cleanup;
    approved=calloc(count,sizeof(*approved));if(!approved)goto cleanup;
    for(DWORD i=0;i<count;++i) {
        approved[i]=read_path(pipe);
        if(!approved[i]||!absolute_xml(approved[i]))goto cleanup;
        total+=(wcslen(approved[i])+1)*sizeof(wchar_t);if(total>64*1024*1024)goto cleanup;
    }
    qsort(approved,count,sizeof(*approved),compare_paths);
    DWORD acknowledged=0;if(!transfer(pipe,&acknowledged,sizeof(acknowledged),TRUE))goto cleanup;
    for(DWORD requests=0;requests<100000;++requests) {
        wchar_t *path=read_path(pipe);if(!path){result=0;break;}
        struct Reply reply={ERROR_ACCESS_DENIED,0};HANDLE remote=NULL;
        if(bsearch(&path,approved,count,sizeof(*approved),compare_paths)) {
            HANDLE file=open_preset(path);
            if(file!=INVALID_HANDLE_VALUE) {
                // Give the ordinary backend a handle; it performs the verified edit itself.
                if(DuplicateHandle(GetCurrentProcess(),file,client,&remote,0,FALSE,DUPLICATE_SAME_ACCESS)) {
                    reply.error=0;reply.handle=(uint64_t)(uintptr_t)remote;
                }else reply.error=GetLastError();
                CloseHandle(file);
            }else reply.error=GetLastError();
        }
        free(path);
        if(!transfer(pipe,&reply,sizeof(reply),TRUE)) {
            if(remote)DuplicateHandle(client,remote,NULL,NULL,0,FALSE,DUPLICATE_CLOSE_SOURCE);
            break;
        }
    }
cleanup:
    if(approved){for(DWORD i=0;i<count;++i)free(approved[i]);free(approved);}
    CloseHandle(client);CloseHandle(pipe);return result;
}
#else
struct Batch {HANDLE pipe;HANDLE process;};
CAB_AUTH_API void cab_windows_authorization_close(void *handle) {
    struct Batch *batch=handle;if(!batch)return;
    if(batch->pipe!=INVALID_HANDLE_VALUE)CloseHandle(batch->pipe);
    if(batch->process){WaitForSingleObject(batch->process,2000);CloseHandle(batch->process);}
    free(batch);
}
static PSECURITY_DESCRIPTOR pipe_security(void) {
    HANDLE token=NULL;DWORD bytes=0;TOKEN_USER *user=NULL;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return NULL;
    GetTokenInformation(token,TokenUser,NULL,0,&bytes);user=malloc(bytes);
    if(user&&GetTokenInformation(token,TokenUser,user,bytes,&bytes)&&ConvertSidToStringSidW(user->User.Sid,&sid)) {
        wchar_t sddl[512];swprintf(sddl,512,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%ls)",sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,NULL);
    }
    if(sid)LocalFree(sid);
    free(user);CloseHandle(token);return descriptor;
}
CAB_AUTH_API void *cab_windows_authorize(const wchar_t *helper,const wchar_t *const *paths,DWORD count,DWORD *status) {
    *status=ERROR_INVALID_PARAMETER;if(!count||count>MAX_FILES)return NULL;
    size_t total=0;for(DWORD i=0;i<count;++i) {
        if(!paths[i]||!absolute_xml(paths[i])||wcslen(paths[i])>=MAX_PATH_CHARS)return NULL;
        total+=(wcslen(paths[i])+1)*sizeof(wchar_t);if(total>64*1024*1024)return NULL;
    }
    struct Batch *batch=calloc(1,sizeof(*batch));if(!batch){*status=ERROR_OUTOFMEMORY;return NULL;}
    batch->pipe=INVALID_HANDLE_VALUE;GUID guid;wchar_t unique[40],name[128],parameters[192];
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(&guid,unique,40)){*status=ERROR_GEN_FAILURE;goto fail;}
    swprintf(name,128,PIPE_PREFIX L"%ls",unique);
    PSECURITY_DESCRIPTOR descriptor=pipe_security();if(!descriptor){*status=GetLastError();goto fail;}
    SECURITY_ATTRIBUTES security={sizeof(security),descriptor,FALSE};
    batch->pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&security);
    LocalFree(descriptor);if(batch->pipe==INVALID_HANDLE_VALUE){*status=GetLastError();goto fail;}
    swprintf(parameters,192,L"--privileged-helper --pipe \"%ls\" --parent %lu",name,(unsigned long)GetCurrentProcessId());
    SHELLEXECUTEINFOW execute={0};execute.cbSize=sizeof(execute);execute.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    execute.lpVerb=L"runas";execute.lpFile=helper;execute.lpParameters=parameters;execute.nShow=SW_HIDE;
    HRESULT initialized=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    BOOL launched=ShellExecuteExW(&execute);DWORD launch_error=GetLastError();if(SUCCEEDED(initialized))CoUninitialize();
    if(!launched){*status=launch_error;goto fail;}batch->process=execute.hProcess;
    OVERLAPPED connect={0};connect.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!connect.hEvent){*status=GetLastError();goto fail;}
    BOOL connected=ConnectNamedPipe(batch->pipe,&connect);DWORD error=GetLastError();
    if(!connected&&error==ERROR_PIPE_CONNECTED)connected=TRUE;
    else if(!connected&&error==ERROR_IO_PENDING) {
        HANDLE events[]={connect.hEvent,batch->process};
        if(WaitForMultipleObjects(2,events,FALSE,IO_TIMEOUT)==WAIT_OBJECT_0) {
            DWORD bytes=0;connected=GetOverlappedResult(batch->pipe,&connect,&bytes,FALSE);
        }
        if(!connected){CancelIoEx(batch->pipe,&connect);DWORD ignored=0;GetOverlappedResult(batch->pipe,&connect,&ignored,TRUE);}
    }
    CloseHandle(connect.hEvent);if(!connected){*status=ERROR_PIPE_NOT_CONNECTED;goto fail;}
    ULONG client=0;
    if(!GetNamedPipeClientProcessId(batch->pipe,&client)||client!=GetProcessId(batch->process)){*status=ERROR_ACCESS_DENIED;goto fail;}
    if(!transfer(batch->pipe,&count,sizeof(count),TRUE)){*status=GetLastError();goto fail;}
    for(DWORD i=0;i<count;++i) {
        DWORD size=(DWORD)wcslen(paths[i]);
        if(!transfer(batch->pipe,&size,sizeof(size),TRUE)||!transfer(batch->pipe,(void *)paths[i],size*sizeof(wchar_t),TRUE)){*status=GetLastError();goto fail;}
    }
    DWORD acknowledged=ERROR_GEN_FAILURE;
    if(!transfer(batch->pipe,&acknowledged,sizeof(acknowledged),FALSE)||acknowledged){*status=ERROR_INVALID_DATA;goto fail;}
    *status=ERROR_SUCCESS;return batch;
fail:
    cab_windows_authorization_close(batch);return NULL;
}
CAB_AUTH_API uintptr_t cab_windows_authorized_open(void *handle,const wchar_t *path) {
    struct Batch *batch=handle;DWORD size=(DWORD)wcslen(path);struct Reply reply={ERROR_GEN_FAILURE,0};
    if(!size||size>=MAX_PATH_CHARS||!transfer(batch->pipe,&size,sizeof(size),TRUE)||
       !transfer(batch->pipe,(void *)path,size*sizeof(wchar_t),TRUE)||!transfer(batch->pipe,&reply,sizeof(reply),FALSE))return 0;
    if(reply.error||!reply.handle){SetLastError(reply.error?reply.error:ERROR_INVALID_HANDLE);return 0;}
    return (uintptr_t)reply.handle;
}
#endif
