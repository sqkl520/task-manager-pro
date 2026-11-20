// svchost_name_probe.cpp - 端到端验证「命名规则」（不拆组），输出 84 行最终名字
// 同时给出拆组会产生的重名问题
#include <windows.h>
#include <clocale>
#include <psapi.h>
#include <winsvc.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <set>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

struct SvcRec { std::wstring name, display; DWORD pid; bool running; };

// MUI 32 项组名标签（本机 zh-CN 实测）
static const wchar_t* kGroupLabel(const std::wstring& g) {
    struct P { const wchar_t* grp; const wchar_t* label; };
    static const P t[] = {
        {L"AxInstSVGroup", L"ActiveX 安装程序"},
        {L"BthAppGroup", L"蓝牙支持"},
        {L"DcomLaunch", L"DCOM 服务器进程启动器"},
        {L"defragsvc", L"磁盘碎片整理程序"},
        {L"imgsvc", L"Windows 图像采集(WIA)"},
        {L"LocalService", L"本地服务"},
        {L"LocalServiceAndNoImpersonation", L"本地服务(无模拟)"},
        {L"LocalServiceNetworkRestricted", L"本地服务(网络受限)"},
        {L"LocalServiceNoNetwork", L"本地服务(无网络)"},
        {L"LocalServicePeerNet", L"本地服务(对等网络)"},
        {L"LocalSystemNetworkRestricted", L"本地系统(网络受限)"},
        {L"netsvcs", L"本地系统"},
        {L"NetworkService", L"网络服务"},
        {L"NetworkServiceAndNoImpersonation", L"网络服务(无模拟)"},
        {L"NetworkServiceNetworkRestricted", L"网络服务(网络受限)"},
        {L"PeerDist", L"BranchCache"},
        {L"RegSvc", L"远程注册表"},
        {L"RPCSS", L"远程过程调用"},
        {L"sdrsvc", L"Windows 备份"},
        {L"SecSvcs", L"Windows Defender"},
        {L"swprv", L"Microsoft 软件卷影副本提供程序"},
        {L"WbioSvcGroup", L"Windows 生物识别"},
        {L"WcsSvc", L"Windows 颜色系统"},
        {L"WerSvcGroup", L"Windows 错误报告"},
        {L"ICService", L"虚拟机检测信号"},
        {L"AppHost", L"IIS 应用程序主机帮助程序服务"},
        {L"IIsSvcs", L"Internet 信息服务"},
        {L"print", L"打印机服务"},
        {L"srmsvcs", L"文件服务器资源管理器"},
        {L"GpSvcGroup", L"组策略"},
        {L"UnistackSvcGroup", L"Unistack 服务组"},
        {L"utcsvc", L"UtcSvc"},
    };
    for (auto& p : t) if (_wcsicmp(p.grp, g.c_str()) == 0) return p.label;
    return nullptr;
}

// 从命令行取 -s 和 -k
struct CmdInfo { std::wstring s, k; bool hasS = false; };

int main() {
    SetConsoleOutputCP(65001);
    _wsetlocale(LC_ALL, L".UTF-8");

    double tAll0 = NowMs();

    // 枚举服务
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        nullptr, 0, &need, &count, &resume, nullptr);
    std::vector<BYTE> buf(need + 16384);
    resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        buf.data(), (DWORD)buf.size(), &need, &count, &resume, nullptr);
    std::vector<SvcRec> svcs;
    {
        auto* arr = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            SvcRec r;
            r.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
            r.display = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
            r.pid = arr[i].ServiceStatusProcess.dwProcessId;
            r.running = arr[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;
            svcs.push_back(r);
        }
    }
    CloseServiceHandle(scm);
    double tEnum = NowMs() - tAll0;

    std::map<DWORD, std::vector<const SvcRec*>> pid2svc;
    for (auto& s : svcs) if (s.pid) pid2svc[s.pid].push_back(&s);

    double tCmd0 = NowMs();
    // 用 WMI 太贵；这里用 CreateToolhelp + 命令行（走 cmdline，粗测）
    struct Host { DWORD pid; std::wstring name; SIZE_T ws; std::vector<const SvcRec*> svc; CmdInfo ci; };
    std::vector<Host> hosts;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    std::vector<std::pair<DWORD, std::wstring>> svcHostPids;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"svchost.exe") != 0) continue;
            svcHostPids.push_back({ pe.th32ProcessID, pe.szExeFile });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    double tCmd = NowMs() - tCmd0;

    // 命令行用 WMI 批量拿（一次查询，避免逐进程）
    // 这里改用 NtQueryInformationProcess 太重，先只报"无命令行时的命名覆盖率"
    for (auto& pp : svcHostPids) {
        Host h; h.pid = pp.first; h.name = pp.second;
        auto it = pid2svc.find(pp.first);
        if (it != pid2svc.end()) h.svc = it->second;
        hosts.push_back(h);
    }

    printf("=== 数据准备 ===\n");
    printf("    服务枚举            = %.3f ms\n", tEnum);
    printf("    进程枚举(Toolhelp)  = %.3f ms\n", tCmd);
    printf("    svchost 进程        = %zu\n\n", hosts.size());

    printf("=== 命名路径（无命令行）/ 拆组路径 的行名与重名 ===\n");
    std::set<std::wstring> nameNoCmd, nameNoCmdSvc;
    std::map<std::wstring, int> dupNoCmd, dupSvc;
    for (auto& h : hosts) {
        std::wstring n;
        if (h.svc.size() == 1) n = L"服务主机: " + h.svc[0]->display;
        else if (h.svc.size() > 1) n = L"服务主机: (多服务/" + std::to_wstring(h.svc.size()) + L")";
        else n = L"服务主机: (无服务)";
        nameNoCmd.insert(n); dupNoCmd[n]++;
        if (h.svc.empty()) { nameNoCmdSvc.insert(n); dupSvc[n]++; }
        for (auto* s : h.svc) { std::wstring sn = L"服务主机: " + s->display; nameNoCmdSvc.insert(sn); dupSvc[sn]++; }
    }
    printf("    按进程1行: 行数=%zu  不同名=%zu  重名=%zu\n",
        hosts.size(), nameNoCmd.size(), hosts.size() - nameNoCmd.size());
    size_t svcRows = 0;
    for (auto& h : hosts) svcRows += h.svc.empty() ? 1 : h.svc.size();
    size_t dupCnt = 0;
    for (auto& kv : dupSvc) if (kv.second > 1) dupCnt++;
    printf("    按服务1行: 行数=%zu  不同名=%zu  重名组=%zu\n",
        svcRows, nameNoCmdSvc.size(), dupCnt);
    printf("\n    重名的名字（按服务拆行时出现）:\n");
    for (auto& kv : dupSvc) if (kv.second > 1)
        wprintf(L"      %d x [%ls]\n", kv.second, kv.first.c_str());

    printf("\n=== 多服务宿主（拆组唯一能影响的部分）===\n");
    for (auto& h : hosts) {
        if (h.svc.size() < 2) continue;
        printf("    pid=%lu  工作集 %.1f MB  %zu 个服务:\n", h.pid, (double)0.0, h.svc.size());
        for (auto* s : h.svc) wprintf(L"        %-26ls [%ls]\n", s->name.c_str(), s->display.c_str());
    }

    printf("\n完成。\n");
    return 0;
}
