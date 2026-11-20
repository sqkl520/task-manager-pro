// desc.cpp - 统计有多少进程拿不到 FileDescription（复刻 GetFileDescription）
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winver.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <set>
#include <map>
#pragma comment(lib,"version.lib")
#pragma comment(lib,"ntdll.lib")
typedef NTSTATUS (NTAPI *NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessIdInformation = 88;
struct SPIDI { HANDLE ProcessId; UNICODE_STRING ImageName; };
struct US { USHORT Length, MaximumLength; PWSTR Buffer; };
struct SPI { ULONG Next; ULONG Threads; LARGE_INTEGER WSPrivate; ULONG HardFault, ThreadsHigh, CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime; US ImageName; LONG BasePriority; HANDLE Pid, ParentPid;
    ULONG HandleCount, SessionId; ULONG_PTR Key; SIZE_T PeakVirt, Virt, PageFault, PeakWS, WS;
    LARGE_INTEGER PagedPool, NonPagedPool, QuotaPaged, QuotaNon, Pagefile; ULONG PeakPagefile;
    LARGE_INTEGER ReadTransfer, WriteTransfer, OtherTransfer; ULONG ReadOps, WriteOps, OtherOps; };
static NtQSI_t g_fn;
static std::wstring NtToDos(const std::wstring& nt){
    if(nt.size()<2) return L"";
    // \Device\HarddiskVolumeN\... -> query drive letters
    size_t p=nt.rfind(L'\\\\');
    (void)p;
    for(wchar_t c=L'A';c<=L'Z';++c){
        wchar_t dev[16]={0}; wchar_t target[512]={0};
        swprintf_s(dev,L"%c:",c);
        if(QueryDosDeviceW(dev,target,512)){
            size_t tl=wcslen(target);
            if(nt.size()>tl && _wcsnicmp(nt.c_str(),target,tl)==0){
                return std::wstring(dev)+nt.substr(tl);
            }
        }
    }
    return L"";
}
static std::wstring Desc(const std::wstring& path){
    if(path.empty()) return L"";
    DWORD dummy=0; DWORD sz=GetFileVersionInfoSizeW(path.c_str(),&dummy);
    if(sz==0||sz>=4*1024*1024) return L"";
    std::vector<BYTE> buf(sz);
    if(!GetFileVersionInfoW(path.c_str(),0,sz,buf.data())) return L"";
    struct L{WORD a,b;}; L* tr=nullptr; UINT ts=0;
    if(!VerQueryValueW(buf.data(),L"\\VarFileInfo\\Translation",(LPVOID*)&tr,&ts)||!tr||ts<sizeof(L)) return L"";
    wchar_t sub[128]; swprintf_s(sub,L"\\StringFileInfo\\%04x%04x\\FileDescription",tr[0].a,tr[0].b);
    wchar_t* v=nullptr; UINT vs=0;
    if(!VerQueryValueW(buf.data(),sub,(LPVOID*)&v,&vs)||!v||vs==0) return L"";
    return std::wstring(v);
}
int wmain(){
    g_fn=(NtQSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
    ULONG sz=8<<20,need=0; std::vector<BYTE> buf(sz);
    for(;;){ NTSTATUS st=g_fn(5,buf.data(),sz,&need); if(st==0)break; sz=need?need+65536:sz*2; buf.resize(sz);} 
    auto* cur=(SPI*)buf.data();
    std::vector<std::pair<unsigned,std::wstring>> procs;
    for(;;){ procs.push_back({(unsigned)(uintptr_t)cur->Pid, cur->ImageName.Buffer&&cur->ImageName.Length? std::wstring(cur->ImageName.Buffer,cur->ImageName.Length/2):L"?"});
      if(!cur->Next)break; cur=(SPI*)((BYTE*)cur+cur->Next);} 
    int total=0,noPath=0,noVer=0,noDesc=0,ok=0;
    std::map<std::wstring,int> descCount;
    std::vector<std::wstring> noDescNames, noVerNames;
    for(auto&pr:procs){
        if(pr.first==0) continue;
        ++total;
        wchar_t nbuf[1024]={};
        SPIDI info{}; info.ProcessId=(HANDLE)(uintptr_t)pr.first;
        info.ImageName.Length=0; info.ImageName.MaximumLength=sizeof(nbuf); info.ImageName.Buffer=nbuf;
        ULONG needed=0; NTSTATUS st=g_fn(kSystemProcessIdInformation,&info,sizeof(info),&needed);
        if(st<0 || info.ImageName.Length==0){ ++noPath; continue; }
        std::wstring nt(info.ImageName.Buffer, info.ImageName.Length/2);
        std::wstring dos=NtToDos(nt);
        if(dos.empty()){ ++noPath; continue; }
        DWORD dummy=0; DWORD sz2=GetFileVersionInfoSizeW(dos.c_str(),&dummy);
        if(sz2==0){ ++noVer; noVerNames.push_back(pr.second); continue; }
        std::wstring d=Desc(dos);
        if(d.empty()){ ++noDesc; noDescNames.push_back(pr.second); continue; }
        ++ok; descCount[d]++;
    }
    printf("totalProcs(pid!=0)=%d  withDesc=%d  noVersionInfo=%d  hasVerButNoDesc=%d  noPath/emptyPath=%d\n",
       total,ok,noVer,noDesc,noPath);
    printf("\n=== exe 无版本信息（%d）===\n",noVer);
    for(size_t i=0;i<noVerNames.size() && i<40;++i) wprintf(L"  %ls\n",noVerNames[i].c_str());
    printf("\n=== 有版本信息但无 FileDescription（%d）===\n",noDesc);
    for(size_t i=0;i<noDescNames.size() && i<40;++i) wprintf(L"  %ls\n",noDescNames[i].c_str());
    printf("\n=== 描述重复（同一描述被多个进程用）===\n");
    for(auto&kv:descCount) if(kv.second>1) wprintf(L"  %d x [%ls]\n",kv.second,kv.first.c_str());
    return 0;
}
