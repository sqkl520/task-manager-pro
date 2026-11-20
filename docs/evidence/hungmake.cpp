// hungmake.cpp - 制造一个真正的无响应窗口（不泵消息 > 6 秒），供 resp.exe 检测
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <cstdio>
LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){ return DefWindowProcW(h,m,w,l); }
int wmain(){
  WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WP; wc.hInstance=GetModuleHandleW(nullptr);
  wc.lpszClassName=L"HungTestWnd"; wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
  RegisterClassExW(&wc);
  HWND h=CreateWindowExW(0,L"HungTestWnd",L"HUNG PROBE WINDOW",WS_OVERLAPPEDWINDOW,
      100,100,400,300,nullptr,nullptr,wc.hInstance,nullptr);
  ShowWindow(h,SW_SHOW);
  // 泵 1 秒让窗口正常出现
  DWORD t0=GetTickCount();
  MSG msg;
  while(GetTickCount()-t0 < 1000){ while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){ TranslateMessage(&msg); DispatchMessageW(&msg);} Sleep(10); }
  printf("window up pid=%lu hwnd=%p; now blocking 9s without pumping\n",(unsigned long)GetCurrentProcessId(),h);
  fflush(stdout);
  Sleep(9000);   // 不泵消息 → 系统标记为 hung
  printf("done\n");
  return 0;
}
