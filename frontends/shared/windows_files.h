#pragma once
/* Relative opens bind every component to an already opened directory. */
#include <windows.h>
#include <winternl.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 3
#endif
static inline HANDLE cab_open_child(HANDLE parent, const wchar_t *name, ACCESS_MASK access,
                                   ULONG disposition, ULONG options, ULONG sharing) {
    typedef NTSTATUS (NTAPI *Open)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
        PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    typedef ULONG (WINAPI *MapError)(NTSTATUS);
    Open open_fn=NULL; MapError map_fn=NULL;
    FARPROC symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile");
    memcpy(&open_fn,&symbol,sizeof(open_fn));
    symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError");
    memcpy(&map_fn,&symbol,sizeof(map_fn));
    size_t length=wcslen(name);
    if(!open_fn||!map_fn||!length||length>32766||wcspbrk(name,L"\\/:")||
       !wcscmp(name,L".")||!wcscmp(name,L"..")){SetLastError(ERROR_INVALID_NAME);return INVALID_HANDLE_VALUE;}
    UNICODE_STRING unicode={(USHORT)(length*sizeof(wchar_t)),(USHORT)(length*sizeof(wchar_t)),(PWSTR)name};
    OBJECT_ATTRIBUTES attributes;memset(&attributes,0,sizeof(attributes));
    InitializeObjectAttributes(&attributes,&unicode,OBJ_CASE_INSENSITIVE,parent,NULL);
    IO_STATUS_BLOCK result; HANDLE file=NULL;
    NTSTATUS status=open_fn(&file,access|SYNCHRONIZE,&attributes,&result,NULL,FILE_ATTRIBUTE_NORMAL,
        sharing,disposition,options|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
    if(status<0){SetLastError(map_fn(status));return INVALID_HANDLE_VALUE;}
    BY_HANDLE_FILE_INFORMATION info;
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(file);SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;
    }
    return file;
}
static inline HANDLE cab_open_directory(const wchar_t *path, int create) {
    size_t length=wcslen(path),begin=0; wchar_t *copy=_wcsdup(path);
    if(!copy){SetLastError(ERROR_OUTOFMEMORY);return INVALID_HANDLE_VALUE;}
    for(size_t i=0;i<length;++i)if(copy[i]==L'/')copy[i]=L'\\';
    if(length>=3&&copy[1]==L':'&&copy[2]==L'\\')begin=3;
    else if(length>4&&copy[0]==L'\\'&&copy[1]==L'\\'&&copy[2]!=L'?'&&copy[2]!=L'.') {
        wchar_t *server=wcschr(copy+2,L'\\');
        wchar_t *share=server?wcschr(server+1,L'\\'):NULL;
        if(server&&server>copy+2)begin=share?(size_t)(share-copy):length;
    }
    if(!begin){free(copy);SetLastError(ERROR_INVALID_NAME);return INVALID_HANDLE_VALUE;}
    wchar_t saved=copy[begin];copy[begin]=0;
    HANDLE directory=CreateFileW(copy,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    copy[begin]=saved;
    if(directory!=INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        if(!GetFileInformationByHandle(directory,&info)||
           !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(directory);directory=INVALID_HANDLE_VALUE;SetLastError(ERROR_ACCESS_DENIED);
        }
    }
    while(directory!=INVALID_HANDLE_VALUE&&begin<length) {
        if(copy[begin]==L'\\'){++begin;continue;}
        size_t end=begin;while(end<length&&copy[end]!=L'\\')++end;
        wchar_t stop=copy[end];copy[end]=0;
        HANDLE next=cab_open_child(directory,copy+begin,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
            create?FILE_OPEN_IF:FILE_OPEN,FILE_DIRECTORY_FILE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
        DWORD error=GetLastError();CloseHandle(directory);directory=next;SetLastError(error);
        copy[end]=stop;begin=end;
    }
    DWORD error=GetLastError();free(copy);SetLastError(error);return directory;
}
static inline BOOL cab_publish_handle(HANDLE file, HANDLE parent, const wchar_t *name) {
    typedef NTSTATUS (NTAPI *SetInfo)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
    typedef ULONG (WINAPI *MapError)(NTSTATUS);
    SetInfo set_fn=NULL;MapError map_fn=NULL;
    FARPROC symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile");
    memcpy(&set_fn,&symbol,sizeof(set_fn));
    symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError");
    memcpy(&map_fn,&symbol,sizeof(map_fn));
    size_t length=wcslen(name);
    if(!set_fn||!map_fn||!length||length>32766||wcspbrk(name,L"\\/:")) {SetLastError(ERROR_INVALID_NAME);return FALSE;}
    size_t size=sizeof(FILE_RENAME_INFORMATION)+length*sizeof(wchar_t);
    FILE_RENAME_INFORMATION *info=(FILE_RENAME_INFORMATION *)calloc(1,size);
    if(!info){SetLastError(ERROR_OUTOFMEMORY);return FALSE;}
    info->ReplaceIfExists=FALSE;info->RootDirectory=parent;info->FileNameLength=(ULONG)(length*sizeof(wchar_t));
    memcpy(info->FileName,name,info->FileNameLength);
    IO_STATUS_BLOCK io;
    NTSTATUS status=set_fn(file,&io,info,(ULONG)size,FileRenameInformation);
    free(info);if(status<0){SetLastError(map_fn(status));return FALSE;}return TRUE;
}
