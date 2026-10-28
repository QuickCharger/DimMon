/* ===========================================================================
 *  brightnessctl.exe  —  Windows 显示器亮度控制（DDC/CI + WMI）+ 本地 Web UI
 *  ---------------------------------------------------------------------------
 *  亮度核心从 Monitorian (emoacht/Monitorian) 重写为纯 C++：
 *    DeviceContext.cs        -> EnumDisplayMonitors / GetMonitorInfo
 *    MonitorConfiguration.cs -> dxva2 高层 + 低层 DDC/CI
 *    DdcMonitorItem.cs       -> raw <-> percent 换算、瞬时错误重试
 *    MSMonitor.cs            -> WMI (ROOT\WMI WmiMonitorBrightnessMethods)
 *
 *  命令行:
 *    brightnessctl.exe ui [port]     启动后台服务并打开配置页面（默认随机端口）
 *    brightnessctl.exe list          列出显示器与当前亮度
 *    brightnessctl.exe get           读取所有显示器亮度
 *    brightnessctl.exe set <0-100>   设置所有显示器亮度
 *    brightnessctl.exe max / min     调到最高 / 最低
 *    brightnessctl.exe vcp <c> <v>   写任意 VCP（如 vcp 0x10 100）
 *
 *  构建（cmake + MinGW，不需要 MSVC / Windows SDK）:
 *    cmake -S . -B build -G "MinGW Makefiles"
 *    cmake --build build
 *
 *  依赖: nlohmann/json（header-only，MIT）用于 JSON 序列化。
 *       由 CMake 的 FetchContent 在 configure 阶段下载到 build/_deps/，
 *       源码不进本仓库；国内默认走 ghfast.top 镜像（直连 GitHub 实测超时）。
 *       想完全离线构建：把 json.hpp 放到 third_party/nlohmann/ 下即可跳过下载。
 * =========================================================================== */

#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <utility>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>

/* JSON：nlohmann/json（header-only，由 CMake FetchContent 拉取，见 CMakeLists.txt）
 *
 * 用 ordered_json 而不是 json：
 *   普通 json 的底层是 std::map，dump 出来的键按字母排序（enabled, holdMin,
 *   points, scale, target），落盘的 schedule.json 看起来莫名其妙；
 *   ordered_json 保留插入顺序，就是代码里写的 enabled/scale/points，
 *   人打开配置文件一眼能看懂。 */
#include <nlohmann/json.hpp>
using json = nlohmann::ordered_json;

#include <wincrypt.h>

#include "httpd.h"

#define IDR_UI_INDEX   3001
#define VCP_BRIGHTNESS 0x10

/* ---------------------------------------------------------------------------
 * DDCCI 错误码（MinGW 头文件里不一定都有，补齐）
 * ------------------------------------------------------------------------- */
#ifndef ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA
#define ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA      ((DWORD)0xC0262582L)
#endif
#ifndef ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA
#define ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA         ((DWORD)0xC0262583L)
#endif
#ifndef ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM
#define ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM   ((DWORD)0xC0262585L)
#endif
#ifndef ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE
#define ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE ((DWORD)0xC0262586L)
#endif
#ifndef ERROR_GRAPHICS_MCA_INTERNAL_ERROR
#define ERROR_GRAPHICS_MCA_INTERNAL_ERROR               ((DWORD)0xC0262589L)
#endif
#ifndef ERROR_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED
#define ERROR_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED          ((DWORD)0xC026258AL)
#endif
#ifndef ERROR_GRAPHICS_DDCCI_INVALID_DATA
#define ERROR_GRAPHICS_DDCCI_INVALID_DATA               ((DWORD)0xC026258BL)
#endif

/* DDC 总线会抖，这几类错误只重试一次（照抄 Monitorian 的策略） */
static bool IsTransient(DWORD e)
{
    return e == ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA
        || e == ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA
        || e == ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM
        || e == ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE
        || e == ERROR_GRAPHICS_MCA_INTERNAL_ERROR;
}

static const char *ErrText(DWORD e)
{
    if (e == ERROR_SUCCESS) return "OK";
    switch (e) {
    case ERROR_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA: return "I2C 发送错误(瞬时)";
    case ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA:    return "I2C 接收错误(瞬时)";
    case ERROR_GRAPHICS_DDCCI_INVALID_MESSAGE_CHECKSUM: return "DDC/CI 校验和错误";
    case ERROR_GRAPHICS_DDCCI_MONITOR_RETURNED_INVALID_TIMING_STATUS_BYTE:
        return "DDC/CI 时序状态字节无效";
    case ERROR_GRAPHICS_MCA_INTERNAL_ERROR:          return "显卡内部错误";
    case ERROR_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED:     return "该 VCP 代码不被支持";
    case ERROR_GRAPHICS_DDCCI_INVALID_DATA:          return "DDC/CI 数据无效";
    case ERROR_NOT_SUPPORTED:                        return "显示器不支持 DDC/CI";
    case ERROR_ACCESS_DENIED:                        return "访问被拒绝";
    case ERROR_INVALID_HANDLE:                       return "句柄无效";
    case ERROR_GEN_FAILURE:                          return "设备未正常工作(ERROR_GEN_FAILURE)";
    default: break;
    }
    static char buf[64];
    snprintf(buf, sizeof(buf), "错误码 0x%08lX", (unsigned long)e);
    return buf;
}

/* ---------------------------------------------------------------------------
 * 换算：raw 不一定是 0-100，很多显示器是 0-255
 * ------------------------------------------------------------------------- */
static int RawToPercent(DWORD raw, DWORD lo, DWORD hi)
{
    if (hi <= lo) return (int)raw;
    long long p = (long long)(raw - lo) * 100 / (long long)(hi - lo);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)p;
}
static DWORD PercentToRaw(int pct, DWORD lo, DWORD hi)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (hi <= lo) return (DWORD)pct;
    return lo + (DWORD)((long long)pct * (long long)(hi - lo) / 100);
}

static std::string W2U8(const std::wstring &w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::string ToLower(const std::string &s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

/* ---------------------------------------------------------------------------
 * 显示器枚举
 * ------------------------------------------------------------------------- */
struct DisplayInfo {
    HMONITOR     hMonitor;
    RECT         rect;
    std::wstring device;
    bool         primary;
};

static BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    std::vector<DisplayInfo> *out = (std::vector<DisplayInfo> *)lp;
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (LPMONITORINFO)&mi)) return TRUE;

    DisplayInfo d;
    d.hMonitor = hMon;
    d.rect     = mi.rcMonitor;
    d.device   = mi.szDevice;
    d.primary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    out->push_back(d);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * DDC/CI 读 / 写（带一次瞬时错误重试）
 * ------------------------------------------------------------------------- */
static bool DdcciGetBrightness(HANDLE h, DWORD &lo, DWORD &cur, DWORD &hi, DWORD &err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        DWORD a = 0, b = 0, c = 0;
        if (GetMonitorBrightness(h, &a, &b, &c)) {
            lo = a; cur = b; hi = c; err = ERROR_SUCCESS;
            return true;
        }
        err = GetLastError();
        if (!IsTransient(err)) return false;
        Sleep(30);
    }
    return false;
}

/* 高层读失败时，降级到低层 VCP 0x10 */
static bool VcpGet(HANDLE h, BYTE code, DWORD &cur, DWORD &mx, DWORD &err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        DWORD a = 0, b = 0;
        /* 注意：GetVCPFeatureAndVCPFeatureReply 的返回值就是当前值(VCP 0x10=亮度)，
           这里第二个出参语义为 max，低层 API 用 0 表示未知 */
        if (GetVCPFeatureAndVCPFeatureReply(h, code, NULL, &a, &b)) {
            cur = a; mx = b; err = ERROR_SUCCESS;
            return true;
        }
        err = GetLastError();
        if (!IsTransient(err)) return false;
        Sleep(30);
    }
    return false;
}

static bool DdcciSetBrightness(HANDLE h, DWORD raw, DWORD &err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        /* SetMonitorBrightness 即使底层失败也可能返回 TRUE，调用方必须回读校验 */
        if (SetMonitorBrightness(h, raw)) { err = ERROR_SUCCESS; return true; }
        err = GetLastError();
        if (!IsTransient(err)) return false;
        Sleep(30);
    }
    return false;
}

static bool VcpSet(HANDLE h, BYTE code, DWORD raw, DWORD &err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (SetVCPFeature(h, code, raw)) { err = ERROR_SUCCESS; return true; }
        err = GetLastError();
        if (!IsTransient(err)) return false;
        Sleep(30);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * WMI 路径 —— 笔记本内置屏（eDP）只走这条
 * ------------------------------------------------------------------------- */
struct WmiInst {
    std::wstring path;     // __PATH，用于 ExecMethod
    std::wstring name;     // InstanceName
    int          current;  // 当前亮度，-1 表示读不到
};

static void WmiCoInit(bool &needUninit)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    needUninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) needUninit = false;
    CoInitializeSecurity(NULL, -1, NULL, NULL,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);   /* 二次调用返回 TOO_LATE，可忽略 */
}

static std::vector<WmiInst> WmiList(void)
{
    std::vector<WmiInst> out;
    bool uninit = false;
    WmiCoInit(uninit);

    IWbemLocator  *pLoc = NULL;
    IWbemServices *pSvc = NULL;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID *)&pLoc);
    if (SUCCEEDED(hr) && pLoc) {
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        hr = pLoc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
        SysFreeString(ns);
    }
    if (!pSvc) goto done;

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    {
        /* 1) 当前亮度表：InstanceName -> CurrentBrightness */
        std::vector<std::pair<std::wstring, int> > cur;
        IEnumWbemClassObject *e = NULL;
        BSTR lang = SysAllocString(L"WQL");
        BSTR q    = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
        hr = pSvc->ExecQuery(lang, q,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &e);
        SysFreeString(lang); SysFreeString(q);
        if (SUCCEEDED(hr) && e) {
            for (;;) {
                IWbemClassObject *o = NULL;
                ULONG got = 0;
                if (e->Next(WBEM_INFINITE, 1, &o, &got) != S_OK || got == 0 || !o) break;
                VARIANT nm, cb;
                VariantInit(&nm); VariantInit(&cb);
                o->Get(L"InstanceName", 0, &nm, NULL, NULL);
                o->Get(L"CurrentBrightness", 0, &cb, NULL, NULL);
                int v = -1;
                if      (cb.vt == VT_I4)  v = cb.lVal;
                else if (cb.vt == VT_UI1) v = cb.bVal;
                else if (cb.vt == VT_I2)  v = cb.iVal;
                else if (cb.vt == VT_UI2) v = cb.uiVal;
                if (nm.vt == VT_BSTR && nm.bstrVal)
                    cur.push_back(std::make_pair(std::wstring(nm.bstrVal), v));
                VariantClear(&nm); VariantClear(&cb);
                o->Release();
            }
            e->Release();
        }

        /* 2) 可写方法的实例路径 */
        IEnumWbemClassObject *e2 = NULL;
        BSTR lang2 = SysAllocString(L"WQL");
        BSTR q2    = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
        hr = pSvc->ExecQuery(lang2, q2,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &e2);
        SysFreeString(lang2); SysFreeString(q2);
        if (SUCCEEDED(hr) && e2) {
            for (;;) {
                IWbemClassObject *o = NULL;
                ULONG got = 0;
                if (e2->Next(WBEM_INFINITE, 1, &o, &got) != S_OK || got == 0 || !o) break;
                VARIANT nm, pa;
                VariantInit(&nm); VariantInit(&pa);
                o->Get(L"InstanceName", 0, &nm, NULL, NULL);
                o->Get(L"__PATH", 0, &pa, NULL, NULL);
                if (pa.vt == VT_BSTR && pa.bstrVal) {
                    WmiInst wi;
                    wi.path    = pa.bstrVal;
                    wi.current = -1;
                    if (nm.vt == VT_BSTR && nm.bstrVal) {
                        wi.name = nm.bstrVal;
                        for (size_t i = 0; i < cur.size(); ++i)
                            if (cur[i].first == wi.name) wi.current = cur[i].second;
                    }
                    out.push_back(wi);
                }
                VariantClear(&nm); VariantClear(&pa);
                o->Release();
            }
            e2->Release();
        }
    }

done:
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (uninit) CoUninitialize();
    return out;
}

static bool WmiSetBrightness(const std::wstring &path, int percent)
{
    bool uninit = false;
    WmiCoInit(uninit);

    IWbemLocator     *pLoc = NULL;
    IWbemServices    *pSvc = NULL;
    IWbemClassObject *pCls = NULL, *pDef = NULL, *pIn = NULL, *pOut = NULL;
    BSTR meth = NULL;
    bool ok = false;
    HRESULT hr;

    do {
        hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                              IID_IWbemLocator, (LPVOID *)&pLoc);
        if (FAILED(hr) || !pLoc) break;

        BSTR ns = SysAllocString(L"ROOT\\WMI");
        hr = pLoc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
        SysFreeString(ns);
        if (FAILED(hr) || !pSvc) break;

        CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

        BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods");
        hr = pSvc->GetObject(cls, 0, NULL, &pCls, NULL);
        SysFreeString(cls);
        if (FAILED(hr) || !pCls) break;

        meth = SysAllocString(L"WmiSetBrightness");
        hr = pCls->GetMethod(meth, 0, &pDef, NULL);
        if (FAILED(hr) || !pDef) break;

        hr = pDef->SpawnInstance(0, &pIn);
        if (FAILED(hr) || !pIn) break;

        VARIANT t; VariantInit(&t);
        t.vt = VT_I4; t.lVal = 0;                       /* Timeout = 0 */
        pIn->Put(L"Timeout", 0, &t, 0);
        VariantClear(&t);

        VARIANT b; VariantInit(&b);
        b.vt = VT_I4; b.lVal = (LONG)percent;
        pIn->Put(L"Brightness", 0, &b, 0);
        VariantClear(&b);

        hr = pSvc->ExecMethod(const_cast<BSTR>(path.c_str()), meth, 0, NULL, pIn, &pOut, NULL);
        if (SUCCEEDED(hr)) ok = true;
    } while (0);

    if (pOut) pOut->Release();
    if (pIn)  pIn->Release();
    if (pDef) pDef->Release();
    if (pCls) pCls->Release();
    if (meth) SysFreeString(meth);
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (uninit) CoUninitialize();
    return ok;
}

/* ---------------------------------------------------------------------------
 * 统一的显示器模型
 * ------------------------------------------------------------------------- */
struct MonInfo {
    int          id;
    std::string  device;     // \\.\DISPLAY1
    std::string  desc;       // 物理显示器描述
    std::string  method;     // "DDC/CI" / "DDC/CI(低层)" / "WMI"
    bool         isWmi;
    bool         primary;
    HANDLE       hPhys;      // 非 WMI 用
    DWORD        caps;
    DWORD        colorTemps; // GetMonitorCapabilities 的第二个返回值（颜色温度能力）
    bool         hasCaps;
    std::wstring wmiPath;
    std::wstring wmiName;
    DWORD        lo, hi;
    int          percent;    // -1 = 读不到
    bool         ok;
    std::string  error;

    MonInfo() : id(0), isWmi(false), primary(false), hPhys(NULL), caps(0),
                colorTemps(0), hasCaps(false), lo(0), hi(100), percent(-1),
                ok(false) {}
};

static std::vector<MonInfo>     g_mons;
static std::vector<std::vector<PHYSICAL_MONITOR> > g_physKeep;  // 句柄保活
static std::mutex               g_monMtx;

static void CollectMonitors(void)
{
    std::lock_guard<std::mutex> lk(g_monMtx);
    g_mons.clear();
    g_physKeep.clear();

    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(NULL, NULL, EnumProc, (LPARAM)&displays);

    int id = 0;
    for (size_t i = 0; i < displays.size(); ++i) {
        DisplayInfo &d = displays[i];
        DWORD n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(d.hMonitor, &n) || n == 0) continue;

        std::vector<PHYSICAL_MONITOR> pm(n);
        if (!GetPhysicalMonitorsFromHMONITOR(d.hMonitor, n, pm.data())) continue;
        g_physKeep.push_back(pm);   /* 保留句柄到进程结束 */

        for (DWORD k = 0; k < n; ++k) {
            MonInfo m;
            m.id      = id++;
            m.device  = W2U8(d.device);
            m.desc    = W2U8(pm[k].szPhysicalMonitorDescription);
            m.isWmi   = false;
            m.primary = d.primary;
            m.hPhys   = pm[k].hPhysicalMonitor;

            /* 实测结论（2026-10-15，虚拟机 + 真机各一台，probe 命令对照）：
               第三个参数传 NULL 与传真实变量，行为完全一致 ——
               失败时都是设备层错误（ERROR_GEN_FAILURE=31 / ERROR_NOT_SUPPORTED=50），
               Windows 并不会因 NULL 而返回 ERROR_INVALID_PARAMETER。
               所以这里仍传真实变量，并非为了修 bug，而是：
                 1) 严格贴合 API 契约（MSDN 未把该参数标为 optional，不像
                    GetVCPFeatureAndVCPFeatureReply 的 pvct 明确标了 optional）；
                 2) 顺带把颜色温度能力取回来，用于 list 的诊断输出。 */
            m.hasCaps = (GetMonitorCapabilities(m.hPhys, &m.caps,
                                                &m.colorTemps) == TRUE);

            DWORD lo = 0, cur = 0, hi = 0, err = 0;
            if (DdcciGetBrightness(m.hPhys, lo, cur, hi, err)) {
                m.lo = lo; m.hi = hi;
                m.percent = RawToPercent(cur, lo, hi);
                m.ok = true;
                m.method = "DDC/CI";
            } else {
                DWORD c2 = 0, m2 = 0, e2 = 0;
                if (VcpGet(m.hPhys, VCP_BRIGHTNESS, c2, m2, e2)) {
                    m.lo = 0;
                    m.hi = (m2 > 0 ? m2 : 100);
                    m.percent = RawToPercent(c2, m.lo, m.hi);
                    m.ok = true;
                    m.method = "DDC/CI(低层)";
                } else {
                    m.percent = -1;
                    m.ok = false;
                    m.method.clear();
                    m.error = ErrText(err);
                }
            }
            g_mons.push_back(m);
        }
    }

    /* WMI（笔记本内置屏 / 虚拟背光） */
    std::vector<WmiInst> wmis = WmiList();
    for (size_t i = 0; i < wmis.size(); ++i) {
        MonInfo m;
        m.id        = id++;
        m.device    = "内部显示";
        m.desc      = "内置屏幕（WMI）";
        m.method    = "WMI";
        m.isWmi     = true;
        /* 主屏标记：DDC/CI 通道里若已经有一个主显示器，WMI 通道就不再标 [主]。
           早先这里硬编码 true，导致 list 里出现两个 [主]。 */
        {
            bool alreadyPrimary = false;
            for (size_t j = 0; j < g_mons.size(); ++j)
                if (g_mons[j].primary) { alreadyPrimary = true; break; }
            m.primary = !alreadyPrimary;
        }
        m.wmiPath   = wmis[i].path;
        m.wmiName   = wmis[i].name;
        m.lo        = 0;
        m.hi        = 100;
        m.percent   = wmis[i].current;
        m.ok        = (wmis[i].current >= 0);
        if (!m.ok) m.error = "WMI 读取不到当前亮度";
        g_mons.push_back(m);
    }
}

/* 写一个显示器（调用方需已持有 g_monMtx） */
static bool ApplyOne(MonInfo &m, int percent)
{
    if (m.isWmi) {
        if (!WmiSetBrightness(m.wmiPath, percent)) {
            m.error = "WMI 写入失败";
            return false;
        }
        Sleep(120);
        std::vector<WmiInst> again = WmiList();
        for (size_t i = 0; i < again.size(); ++i)
            if (again[i].name == m.wmiName && again[i].current >= 0)
                m.percent = again[i].current;
        if (m.percent < 0) m.percent = percent;   /* 回读不到就采信命令 */
        m.ok = true;
        m.error.clear();
        return true;
    }

    DWORD tgt = PercentToRaw(percent, m.lo, m.hi);
    DWORD werr = 0;
    bool ok = DdcciSetBrightness(m.hPhys, tgt, werr);
    if (!ok) {
        DWORD c3 = 0, m3 = 0, e4 = 0;
        if (VcpGet(m.hPhys, VCP_BRIGHTNESS, c3, m3, e4) && m3 > 0) {
            m.lo = 0; m.hi = m3;
            tgt = PercentToRaw(percent, 0, m3);
            ok = VcpSet(m.hPhys, VCP_BRIGHTNESS, tgt, werr);
            if (ok) m.method = "DDC/CI(低层)";
        }
    }
    if (!ok) { m.error = ErrText(werr); return false; }

    Sleep(120);
    DWORD lo2 = 0, cur2 = 0, hi2 = 0, e5 = 0;
    if (DdcciGetBrightness(m.hPhys, lo2, cur2, hi2, e5)) {
        m.lo = lo2; m.hi = hi2;
        m.percent = RawToPercent(cur2, lo2, hi2);
    } else {
        DWORD c4 = 0, m4 = 0, e6 = 0;
        if (VcpGet(m.hPhys, VCP_BRIGHTNESS, c4, m4, e6)) {
            m.lo = 0; m.hi = (m4 > 0 ? m4 : 100);
            m.percent = RawToPercent(c4, 0, m.hi);
        } else {
            m.percent = percent;   /* 回读失败但命令已下发 */
        }
    }
    m.ok = true;
    m.error.clear();
    return true;
}

/* id < 0 表示全部 */
static void SetMonPercent(int id, int percent)
{
    std::lock_guard<std::mutex> lk(g_monMtx);
    for (size_t i = 0; i < g_mons.size(); ++i) {
        if (id >= 0 && g_mons[i].id != id) continue;
        if (!g_mons[i].ok) continue;
        ApplyOne(g_mons[i], percent);
    }
}

/* ---------------------------------------------------------------------------
 * JSON（nlohmann/json）
 *  ---------------------------------------------------------------------------
 *  以前这里是手写字符串拼接 + find("\"key\"") 取值。能跑，但很难读也很难改：
 *    - 同一个 key 出现在别的嵌套层会误匹配（纯字符串查找，没有层级概念）
 *    - 数组里再嵌对象就解错（靠 find('{')/find('}') 配对，遇到嵌套直接崩）
 *    - 每个字段都要单独写一个 JsonXxxField，加字段就得抄一遍逻辑
 *  换成 nlohmann/json 之后是声明式写法，嵌套多深都不用管。
 * ------------------------------------------------------------------------- */
static unsigned short g_port = 0;

/* dump 的统一出口。
 * 显示器名来自 EDID / 系统接口，偶尔带非法 UTF-8 字节，而 dump() 默认
 * 遇到非法序列会抛 type_error.316 —— 这里换成 U+FFFD 替换字符。 */
static std::string DumpJson(const json &j, int indent = -1)
{
    return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

/* 解析文本。allow_exceptions=false：失败返回 discarded 值，不抛异常。 */
static bool ParseJson(const std::string &txt, json &out)
{
    out = json::parse(txt, nullptr, false);
    return !out.is_discarded();
}

/* 取字段：不存在或类型不对就返回默认值（不抛异常） */
static double JsonNum(const json &j, const char *key, double def)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}

static int JsonInt(const json &j, const char *key, int def)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return (int)it->get<double>();
}

static bool JsonBool(const json &j, const char *key, bool def)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/* 定义在下方「作息曲线」一节，StateJson 需要先引用。
 * withRuntime=true 时多带 target / holdMin 两个运行态字段（只给 /api/state 用，
 * 不写进配置文件、也不出现在 GET /api/schedule 的响应里）。 */
static json SchedStateJson(bool withRuntime = false);

static std::string StateJson(void)
{
    json j;

    {
        std::lock_guard<std::mutex> lk(g_monMtx);
        json arr = json::array();
        for (const MonInfo &m : g_mons) {
            json o = json::object();
            o["id"]      = m.id;
            o["device"]  = m.device;
            o["desc"]    = m.desc;
            o["method"]  = m.method;
            o["percent"] = m.percent;
            o["min"]     = (int)m.lo;
            o["max"]     = (int)m.hi;
            o["primary"] = m.primary;
            o["ok"]      = m.ok;
            o["error"]   = m.error;
            arr.push_back(std::move(o));
        }
        j["ok"]       = true;
        j["port"]     = (int)g_port;
        j["count"]    = (int)arr.size();
        j["monitors"] = std::move(arr);
    }

    /* 作息曲线状态（内部用另一把锁；两把锁从不同时持有，不会死锁） */
    j["schedule"] = SchedStateJson(true);
    return DumpJson(j);
}

/* ---------------------------------------------------------------------------
 * 内嵌 UI 资源（开发模式下 exe 同目录 ui\index.html 优先）
 * ------------------------------------------------------------------------- */
static bool ReadWholeFileW(const std::wstring &path, std::string &out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, NULL);
    if (sz == 0 || sz > 8u * 1024u * 1024u) { CloseHandle(f); return false; }
    out.resize(sz);
    DWORD rd = 0;
    BOOL ok = ReadFile(f, &out[0], sz, &rd, NULL);
    CloseHandle(f);
    if (!ok) return false;
    out.resize(rd);
    return true;
}

/* ---------------------------------------------------------------------------
 *  作息曲线（按 24 小时时间表自动调节亮度）
 *  ---------------------------------------------------------------------------
 *  数据：enabled / scale(10-100) / points[{x:0-24小时, y:0-100亮度}]
 *  求值：点之间线性插值 × scale/100，范围外取端点值（与前端完全一致）
 *  配置：exe 同目录 schedule.json（便携优先），不可写时回退 %APPDATA%
 * ------------------------------------------------------------------------- */
static void Logf(const char *fmt, ...);   /* 前向声明：定义在下方日志一节 */

struct SchPoint { double x; int y; };

static std::mutex            g_schMtx;
static bool                  g_schEnabled = false;
static int                   g_schScale   = 100;
static std::vector<SchPoint> g_schPoints;
static time_t                g_manualUntil = 0;    /* 手动操作后的免打扰截止时刻 */
static int                   g_lastApplied = -1;   /* 上次自动写入的值，用于去重 */
static time_t                g_lastWriteAt = 0;    /* 上次真正写入的时刻，用于最小间隔 */
static int                   g_failStreak  = 0;    /* 连续写入失败次数，用作退避指数 */
static long                  g_nextGap     = 60;   /* 下次写入与上次的最小间隔（秒） */

/* ---------- 路径 ---------- */
static std::wstring ExeDirW(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".\\";
    std::wstring p(exe, n);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L".\\" : p.substr(0, pos + 1);
}

/* 探测目录是否可写：建一个临时文件立刻删掉，不在用户目录留 0 字节垃圾 */
static bool DirWritableW(const std::wstring &dir)
{
    wchar_t tmp[MAX_PATH];
    if (!GetTempFileNameW(dir.c_str(), L"bctl", 0, tmp)) return false;
    DeleteFileW(tmp);
    return true;
}

/* 便携优先：exe 同目录；不可写（如装在 Program Files）时回退 %APPDATA% */
static std::wstring SchedPathW(void)
{
    std::wstring dir = ExeDirW();
    if (DirWritableW(dir)) return dir + L"schedule.json";

    wchar_t app[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d = std::wstring(app, n) + L"\\brightnessctl";
        CreateDirectoryW(d.c_str(), NULL);
        return d + L"\\schedule.json";
    }
    return dir + L"schedule.json";
}

/* ---------- 默认曲线（与网页端「护眼」预设一致） ---------- */
static void SchedSetDefault(void)
{
    std::lock_guard<std::mutex> lk(g_schMtx);
    g_schEnabled = false;
    g_schScale   = 100;
    g_schPoints.clear();
    const double xs[] = { 0,  6,  8, 12, 17, 20, 22, 24 };
    const int    ys[] = { 20, 20, 55, 75, 60, 35, 25, 20 };
    for (int i = 0; i < 8; ++i) {
        SchPoint p; p.x = xs[i]; p.y = ys[i];
        g_schPoints.push_back(p);
    }
}

/* ---------- 序列化 / 反序列化 ---------- */
/* 下面要输出 target / holdMin，用到的求值函数定义在后面的「求值」一节 */
static double NowHours(void);
static int    SchedValueAtLocked(double h);
static int    SchedHoldMinutesLocked(void);

/*
 * 同一份对象既用于 GET /api/schedule（配置），也嵌进 /api/state 的 schedule
 * 字段。后者比前者多 target / holdMin 两个运行态字段 —— 多给前端两个只读
 * 字段无害，所以不做两套，省掉一个分支。
 */
static json SchedStateJson(bool withRuntime)
{
    std::lock_guard<std::mutex> lk(g_schMtx);

    json j = json::object();
    j["enabled"] = g_schEnabled;
    j["scale"]   = g_schScale;

    json pts = json::array();
    for (const SchPoint &p : g_schPoints) {
        json q = json::object();
        double rx = std::round(p.x * 100.0) / 100.0;    /* 保留两位小数 */
        if (rx == std::floor(rx)) q["x"] = (int)rx;     /* 整点写成 0 / 7，不写 0.0 */
        else                      q["x"] = rx;
        q["y"] = p.y;
        pts.push_back(std::move(q));
    }
    j["points"] = std::move(pts);

    if (withRuntime) {
        /* 运行态字段，只给 /api/state 用，不写进配置文件。
           此处已持有 g_schMtx，必须用无锁版本，否则 std::mutex 不可重入会死锁 */
        j["target"]  = SchedValueAtLocked(NowHours());
        j["holdMin"] = SchedHoldMinutesLocked();
    }
    return j;
}

static std::string SchedToJson(void)
{
    return DumpJson(SchedStateJson());
}

static bool SchedFromJson(const json &j)
{
    if (!j.is_object()) return false;

    auto pit = j.find("points");
    if (pit == j.end() || !pit->is_array()) return false;

    std::vector<SchPoint> pts;
    for (const json &e : *pit) {
        if (!e.is_object()) continue;
        double x = JsonNum(e, "x", -1);
        double y = JsonNum(e, "y", -1);
        if (x < 0 || x > 24 || y < 0 || y > 100) continue;   /* 越界的点直接丢掉 */
        pts.push_back(SchPoint{ x, (int)(y + 0.5) });
    }
    if (pts.size() < 2) return false;                        /* 至少两点才成曲线 */

    std::sort(pts.begin(), pts.end(),
              [](const SchPoint &a, const SchPoint &b) { return a.x < b.x; });

    /* 0 点与 24 点是同一时刻：两端同时存在时强制同值，避免跨午夜跳变 */
    if (pts.front().x == 0 && pts.back().x == 24)
        pts.back().y = pts.front().y;

    int scale = JsonInt(j, "scale", 100);
    scale = scale < 10 ? 10 : (scale > 100 ? 100 : scale);

    std::lock_guard<std::mutex> lk(g_schMtx);
    g_schEnabled  = JsonBool(j, "enabled", false);
    g_schScale    = scale;
    g_schPoints   = pts;
    g_lastApplied = -1;      /* 配置变了，下一次强制重新应用 */
    return true;
}

/* ---------- 文件读写 ---------- */
static void SchedLoad(void)
{
    std::wstring path = SchedPathW();
    std::string  txt;
    if (!ReadWholeFileW(path, txt) || txt.empty()) {
        Logf("作息曲线: 无配置文件，使用默认 (%ls)", path.c_str());
        SchedSetDefault();
        return;
    }
    json j;
    if (ParseJson(txt, j) && SchedFromJson(j))
        Logf("作息曲线: 已加载 %ls", path.c_str());
    else {
        Logf("作息曲线: 配置解析失败，回退默认");
        SchedSetDefault();
    }
}

static bool SchedSave(void)
{
    /* 缩进 2 空格：这是给人看、甚至手改的文件，不是网络传输的报文 */
    std::string  txt  = DumpJson(SchedStateJson(), 2);
    std::wstring path = SchedPathW();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        Logf("作息曲线: 保存失败 %ls (err=%lu)",
             path.c_str(), (unsigned long)GetLastError());
        return false;
    }
    DWORD wr = 0;
    WriteFile(f, txt.data(), (DWORD)txt.size(), &wr, NULL);
    CloseHandle(f);
    Logf("作息曲线: 已保存 %ls (%lu 字节)", path.c_str(), (unsigned long)wr);
    return wr == txt.size();
}

/* ---------- 求值 ---------- */
static double NowHours(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour + st.wMinute / 60.0 + st.wSecond / 3600.0;
}

/* 无锁版本：调用者必须已持有 g_schMtx */
static int SchedValueAtLocked(double h)
{
    if (g_schPoints.empty()) return 100;
    const std::vector<SchPoint> &p = g_schPoints;

    double raw;
    if (h <= p.front().x)      raw = p.front().y;
    else if (h >= p.back().x)  raw = p.back().y;
    else {
        raw = p.front().y;
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            if (h >= p[i].x && h <= p[i+1].x) {
                double span = p[i+1].x - p[i].x;
                double t = (span > 0) ? (h - p[i].x) / span : 0.0;
                raw = p[i].y + t * (double)(p[i+1].y - p[i].y);
                break;
            }
        }
    }
    double v = raw * g_schScale / 100.0;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return (int)(v + 0.5);
}

/* 加锁版：外部调用用这个 */
static int SchedValueAt(double h)
{
    std::lock_guard<std::mutex> lk(g_schMtx);
    return SchedValueAtLocked(h);
}

/* 用户手动改过亮度后，自动调节退避一段时间 */
static void SchedNoteManual(int minutes)
{
    std::lock_guard<std::mutex> lk(g_schMtx);
    g_manualUntil = time(NULL) + (time_t)minutes * 60;
    Logf("作息曲线: 检测到手动操作，自动调节退避 %d 分钟", minutes);
}

/* 无锁版本：调用者必须已持有 g_schMtx */
static int SchedHoldMinutesLocked(void)
{
    time_t left = g_manualUntil - time(NULL);
    if (left <= 0) return 0;
    return (int)((left + 59) / 60);
}

/* 手动退避剩余分钟（0 = 未退避） */
static int SchedHoldMinutes(void)
{
    std::lock_guard<std::mutex> lk(g_schMtx);
    return SchedHoldMinutesLocked();
}

/* ---------- 自动调节线程（仅常驻 UI 模式启动） ---------- */
/*
 * 两个节流参数的理由（不加会很难用）：
 *   死区 2% —— 曲线是连续插值的，每 30 秒算一次几乎每次都在变；1% 也写的话
 *              亮度一天会被写几百次，肉眼能看见一格一格跳，DDC/CI 也白白损耗。
 *              差不到 2% 就不动，一天实际写入次数降到几十次。
 *   最小间隔 60 秒 —— 防止目标值在死区边界上抖动时反复写。
 */
#define SCH_DEADBAND 2
#define SCH_MIN_GAP  60

static void SchedThread(void)
{
    Logf("作息曲线: 自动调节线程已启动（每 30 秒检查，变化不足 %d%% 不写）",
         SCH_DEADBAND);
    for (;;) {
        Sleep(30000);

        {
            std::lock_guard<std::mutex> lk(g_schMtx);
            if (!g_schEnabled) continue;              /* 没启用 */
            if (time(NULL) < g_manualUntil) continue; /* 手动优先期内不干预 */
        }

        int    want = SchedValueAt(NowHours());
        int    last;
        time_t lastAt;
        long   gap;
        {
            std::lock_guard<std::mutex> lk(g_schMtx);
            last   = g_lastApplied;
            lastAt = g_lastWriteAt;
            gap    = g_nextGap;
        }
        /* 最小间隔对首次写入之外的所有情况都生效（含失败重试） */
        if (lastAt && time(NULL) - lastAt < gap) continue;   /* 写得太勤 */
        if (last >= 0) {                              /* -1 = 还没写过，首次必写 */
            int diff = want - last;
            if (diff < 0) diff = -diff;
            if (diff < SCH_DEADBAND) continue;        /* 变化太小 */
        }

        /* 每次写之前重新枚举：后台可能已经跑了几小时，
           休眠唤醒 / 拔插显示器之后，启动时那份快照未必还有效 */
        CollectMonitors();
        SetMonPercent(-1, want);                      /* 内部只写 ok 的通道 */

        int ok = 0, all = 0;
        {
            std::lock_guard<std::mutex> lk(g_monMtx);
            for (size_t i = 0; i < g_mons.size(); ++i) {
                if (g_mons[i].ok) ok++;
                all++;
            }
        }
        Logf("作息曲线: 自动调节 %d%% -> %d%%（成功 %d/%d）", last, want, ok, all);

        {
            std::lock_guard<std::mutex> lk(g_schMtx);
            if (ok > 0) {
                g_lastApplied = want;            /* 写成功了，记住这次的值 */
                g_failStreak  = 0;
                g_nextGap     = SCH_MIN_GAP;
            } else {
                /* 一个都没写进去（显示器休眠 / 外接屏关了 / 通道临时失效）：
                   指数退避，别每 30 秒白试一次把日志刷满 */
                g_failStreak++;
                long g = (long)SCH_MIN_GAP * (1L << (g_failStreak > 4 ? 4 : g_failStreak));
                if (g > 900) g = 900;            /* 最长 15 分钟 */
                g_nextGap = g;
                Logf("作息曲线: 写入全部失败，%ld 秒后重试（连续失败 %d 次）",
                     g, g_failStreak);
            }
            g_lastWriteAt = time(NULL);
        }
    }
}

static bool LoadUiHtml(std::string &out)
{
    /* 开发模式：exe 同目录下的 ui\index.html 覆盖内嵌版本 */
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p(exe, n);
        size_t pos = p.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            std::wstring dev = p.substr(0, pos + 1) + L"ui\\index.html";
            if (ReadWholeFileW(dev, out)) return true;
        }
    }

    HRSRC h = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_UI_INDEX),
                            MAKEINTRESOURCEW(10));   /* 10 = RT_RCDATA */
    if (!h) return false;
    HGLOBAL g = LoadResource(NULL, h);
    if (!g) return false;
    const char *p = (const char *)LockResource(g);
    DWORD sz = SizeofResource(NULL, h);
    if (!p || sz == 0) return false;
    out.assign(p, sz);
    return true;
}

/* ---------------------------------------------------------------------------
 * 随机 token（防其他网页 CSRF / DNS rebinding）
 * ------------------------------------------------------------------------- */
static std::string MakeToken(void)
{
    unsigned char buf[16];
    bool ok = false;
    HCRYPTPROV prov = 0;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptGenRandom(prov, (DWORD)sizeof(buf), buf)) ok = true;
        CryptReleaseContext(prov, 0);
    }
    if (!ok) {
        srand((unsigned)GetTickCount() ^ (unsigned)time(NULL));
        for (int i = 0; i < 16; ++i) buf[i] = (unsigned char)(rand() & 0xFF);
    }
    static const char *hex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 16; ++i) {
        s.push_back(hex[buf[i] >> 4]);
        s.push_back(hex[buf[i] & 0x0F]);
    }
    return s;
}

static std::string g_token;

/* ---------------------------------------------------------------------------
 * HTTP 路由
 * ------------------------------------------------------------------------- */
static bool HostIsLocal(const std::string &h)
{
    if (h.empty()) return false;
    std::string s = ToLower(h);
    return s.find("127.0.0.1") != std::string::npos
        || s.find("localhost")  != std::string::npos
        || s.find("[::1]")      != std::string::npos;
}

static HttpResponse Route(const HttpRequest &req)
{
    HttpResponse r;

    /* 只接受本机来源 */
    if (!HostIsLocal(req.host)) {
        r.SetText(403, "forbidden: bad host");
        return r;
    }
    if (!req.origin.empty() && !HostIsLocal(req.origin)) {
        r.SetText(403, "forbidden: bad origin");
        return r;
    }
    /* 所有请求都必须带 token */
    if (req.token != g_token) {
        r.SetText(403, "forbidden: invalid token");
        return r;
    }

    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        std::string html;
        if (!LoadUiHtml(html)) {
            r.SetText(500, "UI resource missing");
            return r;
        }
        r.SetHtml(html);
        return r;
    }

    if (req.method == "GET" && req.path == "/favicon.ico") {
        r.status = 204;
        r.contentType = "image/x-icon";
        return r;
    }

    if (req.path == "/api/state") {
        if (req.method != "GET") { r.SetText(405, "method not allowed"); return r; }
        r.SetJson(StateJson());
        return r;
    }

    if (req.path == "/api/set") {
        if (req.method != "POST") { r.SetText(405, "method not allowed"); return r; }
        json body;
        if (!ParseJson(req.body, body) || !body.is_object()) {
            r.SetText(400, "bad request body");
            return r;
        }
        int id  = JsonInt(body, "id", -1);
        int pct = JsonInt(body, "percent", -1);
        if (pct < 0 || pct > 100) { r.SetText(400, "percent out of range"); return r; }
        SetMonPercent(id, pct);
        SchedNoteManual(30);       /* 手动调过之后 30 分钟内不被自动覆盖 */
        r.SetJson(StateJson());
        return r;
    }

    if (req.path == "/api/schedule") {
        if (req.method == "GET") {
            r.SetJson(SchedToJson());
            return r;
        }
        if (req.method == "POST") {
            json body;
            if (!ParseJson(req.body, body) || !SchedFromJson(body)) {
                r.SetText(400, "bad schedule json");
                return r;
            }
            bool ok = SchedSave();
            json res = json::object();
            res["ok"] = ok;
            if (!ok) res["error"] = "save failed";
            r.SetJson(DumpJson(res));
            return r;
        }
        r.SetText(405, "method not allowed"); return r;
    }

    if (req.path == "/api/quit") {
        if (req.method != "POST") { r.SetText(405, "method not allowed"); return r; }
        r.SetJson("{\"ok\":true}");
        /* 给响应留出发送时间再退出 */
        std::thread([]() { Sleep(600); HttpServeStop(); ExitProcess(0); }).detach();
        return r;
    }

    r.SetText(404, "not found");
    return r;
}

/* ---------------------------------------------------------------------------
 * 命令行：list / get / set / max / min / vcp
 * ------------------------------------------------------------------------- */
static void PrintMonitors(void)
{
    CollectMonitors();
    std::lock_guard<std::mutex> lk(g_monMtx);

    printf("共发现 %d 个显示器通道\n", (int)g_mons.size());
    if (g_mons.empty())
        printf("  （没有可控亮度的显示器：虚拟机/远程桌面会话下属正常）\n");

    for (size_t i = 0; i < g_mons.size(); ++i) {
        MonInfo &m = g_mons[i];
        printf("\n[%d] %s%s\n", m.id + 1, m.desc.c_str(), m.primary ? "  [主]" : "");
        printf("    设备: %s\n", m.device.c_str());
        if (m.hasCaps)
            printf("    能力位: 0x%08lX%s   颜色温度能力: 0x%04lX\n",
                   (unsigned long)m.caps,
                   (m.caps & MC_CAPS_BRIGHTNESS) ? "  含亮度" : "  不含亮度",
                   (unsigned long)m.colorTemps);
        if (m.ok)
            printf("    通路: %s   raw 范围 %lu-%lu   当前 %d%%\n",
                   m.method.c_str(), (unsigned long)m.lo, (unsigned long)m.hi, m.percent);
        else
            printf("    不可用: %s\n", m.error.c_str());
    }
}

static void SetAllAndPrint(int percent)
{
    CollectMonitors();
    SetMonPercent(-1, percent);

    std::lock_guard<std::mutex> lk(g_monMtx);
    printf("目标亮度: %d%%\n", percent);
    int hit = 0;
    for (size_t i = 0; i < g_mons.size(); ++i) {
        MonInfo &m = g_mons[i];
        if (m.ok) {
            printf("  [%d] %-28s %-14s -> %d%%  [成功]\n",
                   m.id + 1, m.desc.c_str(), m.method.c_str(), m.percent);
            hit++;
        } else {
            printf("  [%d] %-28s 跳过（%s）\n", m.id + 1, m.desc.c_str(), m.error.c_str());
        }
    }
    printf(">>> 成功操作 %d 个显示器\n", hit);
}

/* ---------------------------------------------------------------------------
 * probe 子命令：DDC/CI 底层诊断
 *   对每个物理显示器句柄分别用两种写法调 GetMonitorCapabilities，
 *   并逐条试高层 / 低层亮度通路，用来判断"读不到亮度"到底卡在哪一层。
 * ------------------------------------------------------------------------- */
static void RunProbe(void)
{
    CollectMonitors();

    std::lock_guard<std::mutex> lk(g_monMtx);

    printf("\n=== DDC/CI 诊断 ===\n\n");

    size_t total = 0;
    for (size_t i = 0; i < g_physKeep.size(); ++i) total += g_physKeep[i].size();
    printf("物理显示器句柄: %d 个\n\n", (int)total);

    int idx = 0;
    for (size_t i = 0; i < g_physKeep.size(); ++i) {
        for (size_t k = 0; k < g_physKeep[i].size(); ++k) {
            HANDLE h = g_physKeep[i][k].hPhysicalMonitor;
            idx++;
            printf("[%d] %ls\n", idx, g_physKeep[i][k].szPhysicalMonitorDescription);

            /* A：第三个参数传 NULL */
            DWORD capsA = 0;
            SetLastError(0);
            BOOL a = GetMonitorCapabilities(h, &capsA, NULL);
            DWORD eA = GetLastError();
            printf("  能力(A 传 NULL)  : %s  caps=0x%08lX  err=%lu %s\n",
                   a ? "成功" : "失败", (unsigned long)capsA,
                   (unsigned long)eA, ErrText(eA));

            /* B：两个参数都传真实变量 */
            DWORD capsB = 0, tempsB = 0;
            SetLastError(0);
            BOOL b = GetMonitorCapabilities(h, &capsB, &tempsB);
            DWORD eB = GetLastError();
            printf("  能力(B 传变量)   : %s  caps=0x%08lX  色温=0x%04lX  err=%lu %s\n",
                   b ? "成功" : "失败", (unsigned long)capsB,
                   (unsigned long)tempsB, (unsigned long)eB, ErrText(eB));

            if (a != b || eA != eB)
                printf("  >>> 两种写法结果不同 —— 传 NULL 确实改变了行为\n");
            else
                printf("  >>> 两种写法结果一致 —— 此处的失败与参数写法无关\n");

            /* 高层亮度 */
            DWORD lo = 0, cur = 0, hi = 0, err = 0;
            if (DdcciGetBrightness(h, lo, cur, hi, err))
                printf("  高层亮度         : 成功 raw=%lu  (%lu-%lu)\n",
                       (unsigned long)cur, (unsigned long)lo, (unsigned long)hi);
            else
                printf("  高层亮度         : 失败 err=%lu %s\n",
                       (unsigned long)err, ErrText(err));

            /* 低层 VCP 0x10 */
            DWORD c2 = 0, m2 = 0, e2 = 0;
            if (VcpGet(h, VCP_BRIGHTNESS, c2, m2, e2))
                printf("  低层 VCP 0x10    : 成功 raw=%lu  (max=%lu)\n",
                       (unsigned long)c2, (unsigned long)m2);
            else
                printf("  低层 VCP 0x10    : 失败 err=%lu %s\n",
                       (unsigned long)e2, ErrText(e2));
            printf("\n");
        }
    }

    if (total == 0) printf("（没有枚举到任何物理显示器句柄）\n\n");

    /* WMI 通道概要 */
    int wmi = 0;
    for (size_t i = 0; i < g_mons.size(); ++i) {
        if (!g_mons[i].isWmi) continue;
        wmi++;
        printf("[WMI] %s  当前 %d%%  %s\n",
               g_mons[i].desc.c_str(), g_mons[i].percent,
               g_mons[i].ok ? "可用" : ("不可用: " + g_mons[i].error).c_str());
    }
    if (wmi == 0) printf("（没有 WMI 通道）\n");
}

static void DoVcp(BYTE code, DWORD value)
{
    CollectMonitors();
    std::lock_guard<std::mutex> lk(g_monMtx);
    for (size_t i = 0; i < g_mons.size(); ++i) {
        if (g_mons[i].isWmi) continue;
        DWORD err = 0;
        bool ok = VcpSet(g_mons[i].hPhys, code, value, err);
        printf("[%s] 写 VCP 0x%02X = %lu : %s\n",
               g_mons[i].desc.c_str(), code, (unsigned long)value,
               ok ? "OK" : ErrText(err));
    }
}

/* ---------------------------------------------------------------------------
 * ui 子命令：后台服务
 * ------------------------------------------------------------------------- */

/* 启动日志（%TEMP%\brightnessctl.log），静默失败时靠它排查 */
static FILE *g_log = NULL;

static std::wstring LogFilePath(void)
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring p(tmp, n);
    p += L"brightnessctl.log";
    return p;
}

static void LogClose(void)
{
    if (g_log) { fclose(g_log); g_log = NULL; }
}

static void LogOpen(bool reset)
{
    if (g_log) return;
    std::wstring p = LogFilePath();
    /* 注意：不能用 "ccs=UTF-8"，老 msvcrt.dll 不支持，会写出空文件。
       源码本身是 UTF-8，直接以二进制写字节即可。 */
    if (reset) {                      /* 只在确认自己是唯一常驻实例时清空 */
        FILE *f = _wfopen(p.c_str(), L"wb");
        if (f) fclose(f);
    }
    /* 必须用追加方式打开：
       重复运行 exe 时会有第二个进程也来开这个日志文件。若用 "wb" 截断，
       它会把常驻实例已经写好的日志清空，而常驻实例自己的文件指针还在原偏移上，
       再写就留下一段 NUL 空洞（日志里表现为一大片空格）。 */
    g_log = _wfopen(p.c_str(), L"ab");
}

static void Logf(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* 判断控制台是否为本进程独占（= 从资源管理器双击启动） */
static bool IsOwnConsole(void)
{
    DWORD pids[4];
    DWORD cnt = GetConsoleProcessList(pids, 4);
    return cnt <= 1;
}

static void CopyTextToClipboard(const std::wstring &s)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void *p = GlobalLock(h);
        if (p) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

static std::wstring PortFilePath(void)
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring p(tmp, n);
    p += L"brightnessctl_ui.port";
    return p;
}

static void WritePortFile(unsigned short port)
{
    std::wstring path = PortFilePath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u", (unsigned)port);
    DWORD wr = 0;
    WriteFile(f, buf, (DWORD)n, &wr, NULL);
    CloseHandle(f);
}

static unsigned short ReadPortFile(void)
{
    std::wstring path = PortFilePath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char buf[32];
    DWORD rd = 0;
    BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL);
    CloseHandle(f);
    if (!ok) return 0;
    buf[rd] = '\0';
    return (unsigned short)atoi(buf);
}

/* 通知常驻实例「请自己再开一次配置页面」的信号。
 *
 * 为什么不能让第二个进程直接拼 URL？
 *   因为 token 只存在于常驻进程的内存（g_token）里，别的进程拿不到，
 *   端口文件又只记了端口没记 token。第二个进程自己拼出来的地址必然缺 token，
 *   浏览器打开就是 "forbidden: invalid token"。
 *   所以正确做法是发信号，让手里握着 token 的常驻进程自己去拼、自己去打开。 */
#define BRIGHTNESSCTL_OPEN_EVENT L"Local\\brightnessctl_ui_open"

static bool AskRunningInstanceToOpen(void)
{
    HANDLE h = NULL;
    /* 常驻实例可能刚起来、事件还没建好，给 2 秒重试窗口 */
    for (int i = 0; i < 20 && !h; ++i) {
        h = OpenEventW(EVENT_MODIFY_STATE, FALSE, BRIGHTNESSCTL_OPEN_EVENT);
        if (!h) Sleep(100);
    }
    if (!h) return false;
    SetEvent(h);
    CloseHandle(h);
    return true;
}

static void OpenBrowser(unsigned short port)
{
    wchar_t url[192];
    swprintf(url, 192, L"http://127.0.0.1:%u/?token=%hs",
             (unsigned)port, g_token.c_str());
    printf("配置页面: %ls\n", url);
    Logf("URL: %ls", url);

    char nb[8];
    if (GetEnvironmentVariableA("BRIGHTNESSCTL_NO_BROWSER", nb, sizeof(nb)) > 0
        && nb[0] == '1') {
        printf("（已跳过自动打开浏览器）\n");
        return;
    }

    HINSTANCE h = ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) {
        /* 浏览器打不开就明确告诉用户，别再静默失败 */
        CopyTextToClipboard(url);
        Logf("ShellExecuteW 失败, code=%d（地址已复制到剪贴板）", (int)(INT_PTR)h);
        std::wstring msg =
            L"本地服务已经启动，但自动打开浏览器失败了。\n\n"
            L"请手动打开下面的地址（已复制到剪贴板，直接粘贴即可）：\n\n";
        msg += url;
        MessageBoxW(NULL, msg.c_str(), L"brightnessctl 亮度控制",
                    MB_ICONINFORMATION | MB_OK | MB_TOPMOST);
    } else {
        Logf("已调用默认浏览器");
    }
}

static int RunUi(unsigned short wantPort, bool hideConsoleNow)
{
    LogOpen(false);
    Logf("=== brightnessctl 启动 ===");
    if (hideConsoleNow) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);   /* 尽早隐藏，避免长时间闪窗 */
    }

    /* 单实例：已有一个在跑，就让它自己再开一次页面
     * 注意：token 只存在于那个进程的内存里，本进程拿不到，
     * 所以这里不能自己拼 URL（那样会打开一个没有 token 的地址 → forbidden）。
     * 正确做法是发信号让常驻实例自己拼。*/
    HANDLE singleton = CreateMutexW(NULL, TRUE, L"Local\\brightnessctl_ui_singleton");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        Logf("检测到已有实例");
        if (AskRunningInstanceToOpen()) {
            unsigned short p = ReadPortFile();
            if (p > 0) {
                printf("已在运行（端口 %u），已请求它重新打开配置页面...\n", (unsigned)p);
                Logf("复用已有实例，端口 %u", (unsigned)p);
            } else {
                printf("已在运行，已请求它重新打开配置页面...\n");
                Logf("复用已有实例");
            }
        } else {
            Logf("通知已有实例失败（可能是旧版本实例）");
            MessageBoxW(NULL,
                L"brightnessctl 已经在后台运行了，但无法通知它打开页面。\n\n"
                L"请先运行 brightnessctl.exe stop 退出旧实例，再重新打开。",
                L"brightnessctl", MB_ICONINFORMATION | MB_OK | MB_TOPMOST);
        }
        return 0;
    }

    /* 到这里说明自己是唯一的常驻实例：清掉上一轮的日志，从头开始记 */
    LogClose();
    LogOpen(true);
    Logf("=== brightnessctl ui 常驻启动 ===");

    /* 退出信号：brightnessctl.exe stop 会 SetEvent */
    HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, L"Local\\brightnessctl_ui_stop");
    if (hStop) ResetEvent(hStop);
    /* 打开页面信号：重复运行本程序时，由第二个进程 SetEvent */
    HANDLE hOpen = CreateEventW(NULL, TRUE, FALSE, BRIGHTNESSCTL_OPEN_EVENT);
    if (hOpen) ResetEvent(hOpen);

    /* 作息曲线：加载配置并启动自动调节线程（只有常驻模式才需要） */
    SchedLoad();
    std::thread(SchedThread).detach();

    CollectMonitors();
    {
        std::lock_guard<std::mutex> lk(g_monMtx);
        printf("已发现 %d 个显示器通道\n", (int)g_mons.size());
        Logf("发现 %d 个显示器通道", (int)g_mons.size());
        for (size_t i = 0; i < g_mons.size(); ++i)
            Logf("  [%d] %s | %s | %s | %d%%",
                 g_mons[i].id + 1, g_mons[i].desc.c_str(),
                 g_mons[i].method.c_str(), g_mons[i].device.c_str(),
                 g_mons[i].percent);
    }

    g_token = MakeToken();

    unsigned short port = HttpServeStart(Route, wantPort);
    if (port == 0) {
        Logf("服务启动失败: %s", HttpServeLastError());
        std::wstring msg = L"本地服务启动失败：端口被占用或被安全软件拦截。";
        MessageBoxW(NULL, msg.c_str(), L"brightnessctl", MB_ICONERROR | MB_OK | MB_TOPMOST);
        return 4;
    }
    g_port = port;
    WritePortFile(port);
    Logf("服务已启动，端口 %u", (unsigned)port);

    printf("本地服务已启动: http://127.0.0.1:%u  (仅本机可访问)\n", (unsigned)port);
    printf("退出方式: 网页上的「退出程序」按钮，或运行 brightnessctl.exe stop\n");

    OpenBrowser(port);

    Logf("进入后台常驻");
    if (hStop && hOpen) {
        HANDLE hs[2] = { hStop, hOpen };
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {          /* stop */
                Logf("收到退出信号，正常退出");
                break;
            }
            if (w == WAIT_OBJECT_0 + 1) {      /* 再开一次页面 */
                ResetEvent(hOpen);
                Logf("收到重新打开页面的请求");
                OpenBrowser(port);
                continue;
            }
            Logf("等待信号返回 %u，退出", (unsigned)w);
            break;
        }
        return 0;
    }
    if (hStop) {
        WaitForSingleObject(hStop, INFINITE);
        Logf("收到退出信号，正常退出");
        return 0;
    }
    for (;;) Sleep(1000);
    return 0;
}

/* 通知后台实例退出 */
static int StopRunning(void)
{
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\brightnessctl_ui_stop");
    if (!h) {
        printf("没有发现正在运行的后台实例。\n");
        return 1;
    }
    SetEvent(h);
    CloseHandle(h);
    printf("已通知后台实例退出。\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
/* 命令行查看作息曲线配置（只读） */
static int ShowSchedule(void)
{
    SchedLoad();
    printf("配置文件: %s\n", W2U8(SchedPathW()).c_str());
    int hold = SchedHoldMinutes();     /* 单独加锁取，避免和下面的锁重入 */
    if (hold > 0)
        printf("手动优先: 剩余 %d 分钟（此期间自动调节不干预）\n", hold);
    {
        std::lock_guard<std::mutex> lk(g_schMtx);
        printf("自动调节: %s\n", g_schEnabled ? "已启用" : "未启用");
        printf("整体缩放: %d%%\n", g_schScale);
        printf("控制点  : %d 个\n", (int)g_schPoints.size());
        {
            /* 时刻显示成 23:09 这种，比 "23.15 时" 好读 */
            double now = NowHours();
            int    nH  = (int)now;
            int    nM  = (int)((now - nH) * 60.0);
            printf("此刻目标: %d%%   (%02d:%02d)\n",
                   SchedValueAtLocked(now), nH, nM);
        }
        for (size_t i = 0; i < g_schPoints.size(); ++i) {
            int hh = (int)g_schPoints[i].x;
            int mm = (int)((g_schPoints[i].x - hh) * 60.0 + 0.5);
            printf("   %02d:%02d  ->  %d%%\n", hh, mm, g_schPoints[i].y);
        }
    }
    printf("\n修改方式: 直接运行 brightnessctl.exe 打开网页配置界面\n");
    return 0;
}

static void PrintUsage(void)
{
    printf(
        "\n双击本程序 = 后台运行并打开网页配置界面（推荐用法）\n"
        "\n命令行用法:\n"
        "  brightnessctl.exe            后台运行并打开网页配置界面（无参数即可）\n"
        "  brightnessctl ui [port]      同上，可指定端口\n"
        "  brightnessctl stop           退出正在运行的后台实例\n"
        "  brightnessctl list           列出显示器与当前亮度\n"
        "  brightnessctl get            读取所有显示器亮度\n"
        "  brightnessctl set <0-100>    设置所有显示器亮度\n"
        "  brightnessctl max / min      调到最高 / 最低\n"
        "  brightnessctl vcp <c> <v>    写任意 VCP（如 vcp 0x10 100）\n"
        "  brightnessctl probe          诊断：逐条试 DDC/CI 各层通路并打印错误码\n"
        "  brightnessctl schedule       查看作息曲线配置与当前目标亮度（只读）\n"
        "\n启动日志（排查用）: %%TEMP%%\\brightnessctl.log\n");
}

int main(int argc, char **argv)
{
    /* 必须在任何输出之前：关掉 stdout 缓冲，否则重定向到文件时不会刷新 */
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* 判定是否独占控制台（= 从资源管理器双击，可以把黑框藏掉） */
    bool ownConsole = IsOwnConsole();
    const char *cmd = (argc > 1) ? argv[1] : NULL;

    /* 无参数：直接进后台模式（双击 exe 就是这个路径） */
    if (!cmd) {
        return RunUi(0, ownConsole);
    }

    if (!strcmp(cmd, "ui") || !strcmp(cmd, "serve")) {
        unsigned short p = 0;
        if (argc > 2) p = (unsigned short)atoi(argv[2]);
        return RunUi(p, ownConsole);
    }

    printf("brightnessctl — Windows 亮度控制 (DDC/CI + WMI)\n");

    if (!strcmp(cmd, "stop") || !strcmp(cmd, "quit")) {
        return StopRunning();
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        PrintUsage();
        return 0;
    }

    if (!strcmp(cmd, "list") || !strcmp(cmd, "get")) {
        PrintMonitors();
        return 0;
    }
    if (!strcmp(cmd, "set")) {
        if (argc < 3) { printf("用法: brightnessctl set <0-100>\n"); return 2; }
        SetAllAndPrint(atoi(argv[2]));
        return 0;
    }
    if (!strcmp(cmd, "max")) { SetAllAndPrint(100); return 0; }
    if (!strcmp(cmd, "min")) { SetAllAndPrint(0);   return 0; }
    if (!strcmp(cmd, "vcp")) {
        if (argc < 4) { printf("用法: brightnessctl vcp <code> <value>\n"); return 2; }
        DoVcp((BYTE)strtol(argv[2], NULL, 0), (DWORD)strtoul(argv[3], NULL, 0));
        return 0;
    }
    if (!strcmp(cmd, "probe") || !strcmp(cmd, "diag")) {
        RunProbe();
        return 0;
    }
    if (!strcmp(cmd, "schedule") || !strcmp(cmd, "sch")) {
        return ShowSchedule();
    }

    PrintUsage();
    return 1;
}
