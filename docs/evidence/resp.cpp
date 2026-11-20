// resp.cpp - 复刻 EnumWindowsProc 的无响应判定，统计命中与核实
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include <map>
#include <set>
struct Hit { DWORD pid; HWND hwnd; bool hung; bool appRoot; std::wstring title; std::wstring cls; DWORD vis; };
static std::vector<Hit> g_hits;
static int g_total=0, g_vis=0;
BOOL CALLBACK EnumProc(HWND h, LPARAM){
  ++g_total;
  DWORD pid=0; GetWindowThreadProcessId(h,&pid); if(!pid) return TRUE;
  if(!IsWindowVisible(h)) return TRUE;
  ++g_vis;
  bool hung = IsHungAppWindow(h)!=0;
  LONG ex=GetWindowLongW(h,GWL_EXSTYLE);
  LONG st=GetWindowLongW(h,GWL_STYLE);
  bool own = GetWindow(h,GW_OWNER)!=0;
  int tl=GetWindowTextLengthW(h);
  wchar_t t[512]={}; GetWindowTextW(h,t,512);
  wchar_t cn[256]={}; GetClassNameW(h,cn,256);
  bool appRoot = !(ex&WS_EX_TOOLWINDOW) && !(st&WS_CHILD) && !own && tl>0;
  Hit x; x.pid=pid; x.hwnd=h; x.hung=hung; x.appRoot=appRoot; x.title=t; x.cls=cn; x.vis=1;
  g_hits.push_back(x);
  return TRUE;
}
int wmain(){
  EnumWindows(EnumProc,0);
  printf("totalWindows=%d visibleWindows=%d\n", g_total, g_vis);
  std::set<DWORD> nr, roots;
  int hungCount=0, hungAppRoot=0, hungNonRoot=0;
  printf("\n=== HUNG (IsHungAppWindow) ===\n");
  for(auto&h:g_hits) if(h.hung){ ++hungCount; nr.insert(h.pid); if(h.appRoot){++hungAppRoot; roots.insert(h.pid);} else ++hungNonRoot;
    printf("pid=%lu hwnd=%p appRoot=%d title=[%ls] class=[%ls]\n",(unsigned long)h.pid,h.hwnd,(int)h.appRoot,h.title.c_str(),h.cls.c_str()); }
  printf("\nhungWindows=%d hungPids=%zu (appRootPids=%d nonRoot=%d)\n", hungCount, nr.size(), hungAppRoot, hungNonRoot);
  printf("=== appRoot count (5-rule pass) ===\n");
  std::set<DWORD> ar; for(auto&h:g_hits) if(h.appRoot) ar.insert(h.pid);
  printf("appRootPids=%zu\n", ar.size());
  // verify hung pids: process name & responsive check via SendMessageTimeout
  printf("\n=== verify each hung pid ===\n");
  for(DWORD pid: nr){
    HANDLE hp=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    wchar_t name[512]=L"?"; DWORD sz=512;
    if(hp){ QueryFullProcessImageNameW(hp,0,name,&sz); CloseHandle(hp);} 
    // find its top window, test with SendMessageTimeout
    HWND top=nullptr; for(auto&h:g_hits) if(h.pid==pid){ top=h.hwnd; break; }
    DWORD_PTR res=0; LRESULT r=0;
    if(top){ r=SendMessageTimeoutW(top,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,300,&res); }
    printf("pid=%lu name=[%ls] SendMsgTimeout=%d(0=hung/1=ok) ret=%lld\n",(unsigned long)pid,name,(int)(r!=0),(long long)r);
  }
  return 0;
}
