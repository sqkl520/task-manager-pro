// treeprobe.cpp - 复刻 classifier.cpp 的 appRootOf 逻辑，验证聚合口径
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#pragma comment(lib,"ntdll.lib")
#pragma comment(lib,"user32.lib")

typedef NTSTATUS (NTAPI *NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
struct US { USHORT Length, MaximumLength; PWSTR Buffer; };
struct TI { LARGE_INTEGER k,u,c; ULONG wait; PVOID sa; CLIENT_ID cid; LONG pri,base; ULONG ctx,ts,wr; };
struct SPI {
    ULONG Next; ULONG Threads;
    LARGE_INTEGER WSPrivate;
    ULONG HardFault, ThreadsHigh, CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    US ImageName;
    LONG BasePriority;
    HANDLE Pid, ParentPid;
    ULONG HandleCount, SessionId;
    ULONG_PTR Key;
    SIZE_T PeakVirt, Virt, PageFault, PeakWS, WS;
    LARGE_INTEGER PagedPool, NonPagedPool, QuotaPaged, QuotaNon, Pagefile;
    ULONG PeakPagefile;
    LARGE_INTEGER ReadTransfer, WriteTransfer, OtherTransfer;
    ULONG ReadOps, WriteOps, OtherOps;
};

struct Row { unsigned pid, ppid; unsigned long long priv, ws; std::wstring name; };

std::unordered_set<unsigned> g_appRoots;
std::unordered_map<unsigned,std::wstring> g_titles;
std::unordered_map<unsigned,unsigned> g_parent;

struct Ctx { };
BOOL CALLBACK EnumProc(HWND h, LPARAM){
  DWORD pid=0; GetWindowThreadProcessId(h,&pid); if(!pid) return TRUE;
  if(!IsWindowVisible(h)) return TRUE;
  LONG ex=GetWindowLongW(h,GWL_EXSTYLE); if(ex&WS_EX_TOOLWINDOW) return TRUE;
  LONG st=GetWindowLongW(h,GWL_STYLE); if(st&WS_CHILD) return TRUE;
  if(GetWindow(h,GW_OWNER)) return TRUE;
  int tl=GetWindowTextLengthW(h); if(tl<=0) return TRUE;
  g_appRoots.insert(pid);
  wchar_t t[512]={}; GetWindowTextW(h,t,512);
  if(t[0] && !g_titles.count(pid)) g_titles[pid]=t;
  return TRUE;
}
int wmain(int argc, wchar_t** argv){
    auto fn = (NtQSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    ULONG sz = 4<<20; std::vector<BYTE> buf(sz); ULONG need=0;
    for(;;){ NTSTATUS st=fn(5,buf.data(),sz,&need); if(st==0)break; sz = need? need+65536 : sz*2; buf.resize(sz);} 
    auto* cur=(SPI*)buf.data();
    std::vector<Row> rows;
    for(;;){
        Row r; r.pid=(unsigned)(uintptr_t)cur->Pid; r.ppid=(unsigned)(uintptr_t)cur->ParentPid;
        r.priv=(unsigned long long)cur->WSPrivate.QuadPart; r.ws=(unsigned long long)cur->WS;
        if(cur->ImageName.Buffer&&cur->ImageName.Length>0) r.name.assign(cur->ImageName.Buffer,cur->ImageName.Length/2);
        else r.name = r.pid==0?L"Idle":L"System";
        rows.push_back(r);
        if(!cur->Next)break; cur=(SPI*)((BYTE*)cur+cur->Next);
    }
    EnumWindows(EnumProc,0);
    std::unordered_map<unsigned,size_t> idx; for(size_t i=0;i<rows.size();++i) idx[rows[i].pid]=i;
    for(auto&r:rows) g_parent[r.pid]=r.ppid;
    // appRootOf
    std::unordered_map<unsigned,unsigned> rootOf;
    for(auto&r:rows){ if(g_appRoots.count(r.pid)){ rootOf[r.pid]=r.pid; continue; }
      unsigned curp=r.pid, root=0;
      for(int d=0;d<8;d++){ auto it=idx.find(curp); if(it==idx.end())break; unsigned par=rows[it->second].ppid;
        if(par==0||par==curp)break; if(g_appRoots.count(par)){root=par;break;} curp=par; }
      if(root) rootOf[r.pid]=root;
    }
    // aggregate
    std::unordered_map<unsigned, std::pair<unsigned long long,unsigned long long>> agg; // root -> (privSum used, ws-only sum)
    std::unordered_map<unsigned,unsigned> cnt;
    std::unordered_map<unsigned,unsigned long long> aggPrivOnly;
    for(auto&r:rows){ if(r.pid==0)continue; auto it=rootOf.find(r.pid); if(it==rootOf.end())continue;
      unsigned root=it->second; unsigned long long v = r.priv? r.priv : r.ws;
      agg[root].first += v; agg[root].second += r.ws; aggPrivOnly[root]+=r.priv; cnt[root]++; }
    wprintf(L"=== app roots (visible windows) ===\n");
    std::vector<unsigned> roots(g_appRoots.begin(), g_appRoots.end());
    std::sort(roots.begin(), roots.end(), [&](unsigned a,unsigned b){ return agg[a].first>agg[b].first; });
    for(unsigned rt: roots){
      auto it=g_titles.find(rt); std::wstring t = it!=g_titles.end()? it->second : L"?";
      unsigned long long mixed = agg[rt].first, ws = agg[rt].second, priv = aggPrivOnly[rt];
      wprintf(L"root=%u cnt=%u title=[%s] mixed=%.1fMB wsSum=%.1fMB privOnly=%.1fMB\n",
        rt, cnt[rt], t.c_str(), mixed/1048576.0, ws/1048576.0, priv/1048576.0);
    }
    wprintf(L"\n=== total processes=%zu roots=%zu aggregatedPids=%zu ===\n", rows.size(), g_appRoots.size(), rootOf.size());
    // sum over all app aggregates vs physical memory
    unsigned long long allAgg=0; unsigned maxAgg=0; unsigned long long maxVal=0;
    for(auto&kv:agg){ allAgg+=kv.second.first; if(kv.second.first>maxVal){maxVal=kv.second.first;maxAgg=kv.first;} }
    MEMORYSTATUSEX ms{}; ms.dwLength=sizeof(ms); GlobalMemoryStatusEx(&ms);
    wprintf(L"sumOfAllAppAggregates=%.2fGB  physTotal=%.2fGB  maxApp(root %u)=%.2fGB\n",
       allAgg/1073741824.0, ms.ullTotalPhys/1073741824.0, maxAgg, maxVal/1073741824.0);
    return 0;
}
