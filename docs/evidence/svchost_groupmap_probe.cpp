// svchost_groupmap_probe.cpp - MUI 32 项组名标签 vs HKLM\...\Svchost 注册表组名
#include <windows.h>
#include <clocale>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

struct MuiLabel { int id; const wchar_t* name; };

// 从 _probe/mui_33550.txt 抄出的 32 项（id -> 标签正文）
static const MuiLabel kMui[] = {
    {33556, L"ActiveX 安装程序"},
    {33557, L"蓝牙支持"},
    {33558, L"DCOM 服务器进程启动器"},
    {33559, L"磁盘碎片整理程序"},
    {33560, L"Windows 图像采集(WIA)"},
    {33561, L"本地服务"},
    {33562, L"本地服务(无模拟)"},
    {33563, L"本地服务(网络受限)"},
    {33564, L"本地服务(无网络)"},
    {33565, L"本地服务(对等网络)"},
    {33566, L"本地系统(网络受限)"},
    {33567, L"本地系统"},
    {33568, L"网络服务"},
    {33569, L"网络服务(无模拟)"},
    {33570, L"网络服务(网络受限)"},
    {33571, L"BranchCache"},
    {33572, L"远程注册表"},
    {33573, L"远程过程调用"},
    {33574, L"Windows 备份"},
    {33575, L"Windows Defender"},
    {33576, L"Microsoft 软件卷影副本提供程序"},
    {33577, L"Windows 生物识别"},
    {33578, L"Windows 颜色系统"},
    {33579, L"Windows 错误报告"},
    {33580, L"虚拟机检测信号"},
    {33581, L"IIS 应用程序主机帮助程序服务"},
    {33582, L"Internet 信息服务"},
    {33583, L"打印机服务"},
    {33584, L"文件服务器资源管理器"},
    {33585, L"组策略"},
    {33586, L"Unistack 服务组"},
    {33587, L"UtcSvc"},
};

static std::vector<std::wstring> RegGroups() {
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Svchost", 0,
        KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    DWORD i = 0;
    while (true) {
        DWORD cch = 512;
        if (RegEnumValueW(k, i++, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

int main() {
    SetConsoleOutputCP(65001);
    _wsetlocale(LC_ALL, L".UTF-8");

    auto groups = RegGroups();
    printf("注册表 svchost 组名 = %zu 个\n\n", groups.size());

    printf("=== 32 项 MUI 标签 -> 猜测的注册表组名（按已知对应关系）===\n");
    // 已知/可推定的组名映射（id -> group name）
    struct Pair { int id; const wchar_t* grp; };
    static const Pair kMap[] = {
        {33556, L"AxInstSVGroup"},
        {33557, L"BthAppGroup"},
        {33558, L"DcomLaunch"},
        {33559, L"defragsvc"},
        {33560, L"imgsvc"},
        {33561, L"LocalService"},
        {33562, L"LocalServiceAndNoImpersonation"},
        {33563, L"LocalServiceNetworkRestricted"},
        {33564, L"LocalServiceNoNetwork"},
        {33565, L"LocalServicePeerNet"},
        {33566, L"LocalSystemNetworkRestricted"},
        {33567, L"netsvcs"},
        {33568, L"NetworkService"},
        {33569, L"NetworkServiceAndNoImpersonation"},
        {33570, L"NetworkServiceNetworkRestricted"},
        {33571, L"PeerDist"},
        {33572, L"?RegSvc?"},
        {33573, L"RPCSS"},
        {33574, L"sdrsvc"},
        {33575, L"?SecSvcs?"},
        {33576, L"swprv"},
        {33577, L"WbioSvcGroup"},
        {33578, L"?WcsSvc?"},
        {33579, L"WerSvcGroup"},
        {33580, L"ICService"},
        {33581, L"?AppHost?"},
        {33582, L"?IIsSvcs?"},
        {33583, L"print"},
        {33584, L"?srmsvcs?"},
        {33585, L"?GpSvcGroup?"},
        {33586, L"UnistackSvcGroup"},
        {33587, L"utcsvc"},
    };
    int hit = 0, miss = 0;
    for (auto& p : kMap) {
        std::wstring g = p.grp;
        bool optional = !g.empty() && g.front() == L'?';
        std::wstring bare = g;
        if (optional) bare = g.substr(1, g.size() - 2);
        bool found = false;
        for (auto& rg : groups) {
            if (_wcsicmp(rg.c_str(), bare.c_str()) == 0) { found = true; break; }
        }
        const wchar_t* label = L"?";
        for (auto& m : kMui) if (m.id == p.id) label = m.name;
        if (found) { hit++; wprintf(L"    HIT  id=%d  [%ls] -> 组名 %ls\n", p.id, label, bare.c_str()); }
        else       { miss++; wprintf(L"    MISS id=%d  [%ls] -> 猜测 %ls（注册表无此组）\n", p.id, label, bare.c_str()); }
    }
    printf("\n    HIT = %d / %d , MISS = %d\n", hit, hit + miss, miss);

    printf("\n=== 反向：注册表 51 个组名里，有多少能在这 32 项 MUI 标签中找到对应 ===\n");
    printf("    （若 MUI 表是按「服务名」建的，注册表组名应当完全对不上）\n");

    // 统计活跃的 -k 组名（从当前进程命令行拿不到，这里只报注册表侧）
    int grpInMui = 0;
    for (auto& g : groups) {
        // 在 kMap 里找
        bool mapped = false;
        for (auto& p : kMap) {
            std::wstring bare = p.grp;
            if (bare.size() >= 2 && bare.front() == L'?') bare = bare.substr(1, bare.size() - 2);
            if (_wcsicmp(bare.c_str(), g.c_str()) == 0) { mapped = true; break; }
        }
        if (mapped) grpInMui++;
    }
    printf("    注册表组成员的 %zu 个中，有 %d 个出现在那 32 项 MUI 标签映射里\n",
        groups.size(), grpInMui);
    printf("\n完成。\n");
    return 0;
}
