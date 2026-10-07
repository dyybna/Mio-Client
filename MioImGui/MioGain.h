
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>

// BASS 类型/常量：有官方 SDK 就优先用官方的 bass.h，没有就回退到本工程的最小声明
//   （函数一律走 GetProcAddress 动态取，所以只需要类型与常量）
#if defined(__has_include)
#  if __has_include("bass.h")
#    include "bass.h"
#  else
#    include "MioBassMini.h"
#  endif
#else
#  include "MioBassMini.h"
#endif
#include "BassBlob.h" // 内嵌的 bass.dll 字节数组（启动时释放到程序目录）

namespace mio_gain {

// ---------------------------------------------------------------------------
// 1. BASS 动态符号
// ---------------------------------------------------------------------------
typedef BOOL    (WINAPI *t_Init)(int, DWORD, DWORD, HWND, const void*);
typedef BOOL    (WINAPI *t_Free)(void);
typedef BOOL    (WINAPI *t_GetDeviceInfo)(DWORD, BASS_DEVICEINFO*);
typedef BOOL    (WINAPI *t_SetDevice)(DWORD);
typedef BOOL    (WINAPI *t_RecordInit)(int);
typedef BOOL    (WINAPI *t_RecordFree)(void);
typedef BOOL    (WINAPI *t_RecordSetDevice)(DWORD);
typedef BOOL    (WINAPI *t_RecordGetDeviceInfo)(DWORD, BASS_DEVICEINFO*);
typedef HRECORD (WINAPI *t_RecordStart)(DWORD, DWORD, DWORD, RECORDPROC*, void*);
typedef BOOL    (WINAPI *t_RecordGetInfo)(BASS_RECORDINFO*);
typedef HSTREAM (WINAPI *t_StreamCreate)(DWORD, DWORD, DWORD, STREAMPROC*, void*);
typedef DWORD   (WINAPI *t_StreamPutData)(HSTREAM, const void*, DWORD);
typedef BOOL    (WINAPI *t_ChannelPlay)(DWORD, BOOL);
typedef BOOL    (WINAPI *t_ChannelStop)(DWORD);
typedef DWORD   (WINAPI *t_ChannelIsActive)(DWORD);
typedef BOOL    (WINAPI *t_ChannelGetInfo)(DWORD, BASS_CHANNELINFO*);
typedef BOOL    (WINAPI *t_ChannelSetAttribute)(DWORD, DWORD, float);
typedef int     (WINAPI *t_ErrorGetCode)(void);

inline HMODULE hLib = nullptr;
inline t_Init               pInit = nullptr;
inline t_Free               pFree = nullptr;
inline t_GetDeviceInfo      pGetDevInfo = nullptr;
inline t_SetDevice          pSetDevice = nullptr;
inline t_RecordInit         pRecInit = nullptr;
inline t_RecordFree         pRecFree = nullptr;
inline t_RecordSetDevice    pRecSetDevice = nullptr;
inline t_RecordGetDeviceInfo pRecGetDevInfo = nullptr;
inline t_RecordStart        pRecStart = nullptr;
inline t_RecordGetInfo      pRecGetInfo = nullptr;
inline t_StreamCreate       pStreamCreate = nullptr;
inline t_StreamPutData      pStreamPutData = nullptr;
inline t_ChannelPlay        pPlay = nullptr;
inline t_ChannelStop        pStop = nullptr;
inline t_ChannelIsActive    pActive = nullptr;
inline t_ChannelGetInfo     pChanInfo = nullptr;
inline t_ChannelSetAttribute pSetAttr = nullptr;
inline t_ErrorGetCode       pErr = nullptr;

// ---------------------------------------------------------------------------
// 2. 状态
// ---------------------------------------------------------------------------
inline HRECORD g_rec = 0;              // 采集句柄
inline HSTREAM g_out = 0;              // 回放流
inline bool    g_loaded = false;       // bass.dll 加载成功
inline bool    g_running = false;      // 采集/回放任一在跑
inline bool    g_isFloat = false;      // 采集格式是否为 32bit float
inline int     g_inDev = -1;           // 采集设备(BASS 索引，-1=默认)
inline int     g_outDev = -1;          // 输出设备(BASS 索引，-1=默认)
inline DWORD   g_rate = 0;             // 实际采样率
inline DWORD   g_chans = 2;

inline std::atomic<float> g_gain{4.5f};        // 当前倍率（默认=最大档 10 -> 4.5x）
inline std::atomic<bool>  g_monitor{true};     // 是否把处理后的声音送输出
inline std::atomic<int>   g_mode{0};           // 0=Mode1 线性+硬限幅  1=Mode2 线性+软限幅
inline std::atomic<int>   g_levelIdx{10};      // 工作级别 1..10（默认最大档）

inline std::atomic<unsigned> g_meterRms{0};    // 处理后 RMS 0..1000
inline std::atomic<unsigned> g_meterPeak{0};   // 处理后峰值 0..1000
inline std::atomic<unsigned> g_clipCount{0};   // 削波计数（诊断）
inline std::string g_err;                      // 最近一次错误

// ---------------------------------------------------------------------------
// 3. 工作级别 → 倍率（10 档；默认第 5 档 = 2.0x）
// ---------------------------------------------------------------------------
inline float LevelToGain(int lv)
{
    static const float tbl[10] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f };
    if (lv < 1)  lv = 1;
    if (lv > 10) lv = 10;
    return tbl[lv - 1];
}

// ---------------------------------------------------------------------------
// 4. 增益本体：16bit / float 两路，就地处理
// ---------------------------------------------------------------------------
inline void ApplyGain16(short* p, int n, float gain, bool soft)
{
    unsigned long long sum = 0;
    unsigned peak = 0, clips = 0;
    for (int i = 0; i < n; ++i)
    {
        float v;
        if (soft)
        {
            // Mode2：tanh 软限幅（大增益下不刺耳）
            v = tanhf((float)p[i] * gain / 32768.0f) * 32768.0f;
        }
        else
        {
            // Mode1：线性 + 硬限幅
            v = (float)p[i] * gain;
            if (v >  32767.0f) { v =  32767.0f; ++clips; }
            if (v < -32768.0f) { v = -32768.0f; ++clips; }
        }
        p[i] = (short)v;
        const float a = v < 0 ? -v : v;
        if (a > (float)peak) peak = (unsigned)a;
        sum += (unsigned long long)((double)v * (double)v);
    }
    if (n > 0)
    {
        const double rms = sqrt((double)sum / (double)n);
        unsigned rm = (unsigned)(rms * 1000.0 / 32768.0 + 0.5);
        unsigned pm = (unsigned)((unsigned long long)peak * 1000 / 32768);
        g_meterRms.store(rm > 1000 ? 1000 : rm);
        g_meterPeak.store(pm > 1000 ? 1000 : pm);
    }
    if (clips) g_clipCount.fetch_add(clips);
}

inline void ApplyGainF32(float* p, int n, float gain, bool soft)
{
    double sum = 0.0; float peak = 0.0f; unsigned clips = 0;
    for (int i = 0; i < n; ++i)
    {
        float v = p[i] * gain;
        if (soft)      v = tanhf(v);
        else if (v > 0.999969f) { v = 0.999969f; ++clips; }
        else if (v < -1.0f)     { v = -1.0f;     ++clips; }
        p[i] = v;
        const float a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sum += (double)v * (double)v;
    }
    if (n > 0)
    {
        const double rms = sqrt(sum / (double)n);
        unsigned rm = (unsigned)(rms * 1000.0 + 0.5);
        unsigned pm = (unsigned)((double)peak * 1000.0 + 0.5);
        g_meterRms.store(rm > 1000 ? 1000 : rm);
        g_meterPeak.store(pm > 1000 ? 1000 : pm);
    }
    if (clips) g_clipCount.fetch_add(clips);
}

// 前置声明（定义见第 10 节）
inline void Log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// 5. 采集回调（BASS 专用线程，只做增益 + 推流）
// ---------------------------------------------------------------------------
inline BOOL CALLBACK RecProc(HRECORD, const void* buffer, DWORD length, void*)
{
    if (!buffer || !length) return TRUE;

    if (g_monitor.load())
    {
        const float gain = g_gain.load();
        const bool  soft = (g_mode.load() == 1);
        if (g_isFloat) ApplyGainF32((float*)buffer, (int)(length / sizeof(float)), gain, soft);
        else           ApplyGain16((short*)buffer, (int)(length / sizeof(short)), gain, soft);

        // 自激保护：连续多块峰值贴顶 = 采集到的就是自己刚播出去的声音（正反馈），
        // 立刻切断送输出，否则会指数级爆音。
        static int s_sat = 0;
        if (g_meterPeak.load() >= 995 && g_levelIdx.load() > 1)
        {
            if (++s_sat > 20)
            {
                g_monitor.store(false);
                Log("gain: [自激保护] 连续 20 块峰值贴顶，判定为正反馈自激，已自动停止送输出。"
                    "请检查 gain.ini 的 input/output 是否指向同一张卡。");
            }
        }
        else s_sat = 0;

        if (g_out)
            pStreamPutData(g_out, buffer, length);   // 推入回放流
    }
    else
    {
        // 不监听时也要出电平（显示采集有数据）
        if (g_isFloat) ApplyGainF32((float*)buffer, (int)(length / sizeof(float)), 1.0f, false);
        else           ApplyGain16((short*)buffer, (int)(length / sizeof(short)), 1.0f, false);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 6. 加载 / 卸载
// ---------------------------------------------------------------------------
inline void StopAll();   // 前置声明（Unload 要用，定义在下面第 8 节）

inline void SetErr(const char* what)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s (BASS err=%d)", what, pErr ? pErr() : 0);
    g_err = buf;
}

inline bool Load(const wchar_t* dllPath)
{
    if (g_loaded) return true;
    g_err.clear();
    hLib = LoadLibraryW(dllPath);
    if (!hLib) { g_err = "找不到 bass.dll（应与 MioImGui.exe 同目录）"; return false; }

#define MIO_BASS_BIND(var, strname, type) do { \
        var = (type)GetProcAddress(hLib, strname); \
        if (!var) { g_err = "bass.dll 缺少导出: " strname; FreeLibrary(hLib); hLib = nullptr; return false; } \
    } while (0)
    MIO_BASS_BIND(pInit,               "BASS_Init",                t_Init);
    MIO_BASS_BIND(pFree,               "BASS_Free",                t_Free);
    MIO_BASS_BIND(pGetDevInfo,         "BASS_GetDeviceInfo",       t_GetDeviceInfo);
    MIO_BASS_BIND(pSetDevice,          "BASS_SetDevice",           t_SetDevice);
    MIO_BASS_BIND(pRecInit,            "BASS_RecordInit",          t_RecordInit);
    MIO_BASS_BIND(pRecFree,            "BASS_RecordFree",          t_RecordFree);
    MIO_BASS_BIND(pRecSetDevice,       "BASS_RecordSetDevice",     t_RecordSetDevice);
    MIO_BASS_BIND(pRecGetDevInfo,      "BASS_RecordGetDeviceInfo", t_RecordGetDeviceInfo);
    MIO_BASS_BIND(pRecStart,           "BASS_RecordStart",         t_RecordStart);
    MIO_BASS_BIND(pRecGetInfo,         "BASS_RecordGetInfo",       t_RecordGetInfo);
    MIO_BASS_BIND(pStreamCreate,       "BASS_StreamCreate",        t_StreamCreate);
    MIO_BASS_BIND(pStreamPutData,      "BASS_StreamPutData",       t_StreamPutData);
    MIO_BASS_BIND(pPlay,               "BASS_ChannelPlay",         t_ChannelPlay);
    MIO_BASS_BIND(pStop,               "BASS_ChannelStop",         t_ChannelStop);
    MIO_BASS_BIND(pActive,             "BASS_ChannelIsActive",     t_ChannelIsActive);
    MIO_BASS_BIND(pChanInfo,           "BASS_ChannelGetInfo",      t_ChannelGetInfo);
    MIO_BASS_BIND(pSetAttr,            "BASS_ChannelSetAttribute", t_ChannelSetAttribute);
    MIO_BASS_BIND(pErr,                "BASS_ErrorGetCode",        t_ErrorGetCode);
#undef MIO_BASS_BIND
    g_loaded = true;
    return true;
}

inline void Unload()
{
    StopAll();
    if (hLib) { FreeLibrary(hLib); hLib = nullptr; }
    g_loaded = false;
    pInit = nullptr; pFree = nullptr; pRecInit = nullptr; pRecFree = nullptr;
    pRecStart = nullptr; pStreamCreate = nullptr;
    pStreamPutData = nullptr; pPlay = nullptr; pStop = nullptr;
}

// ---------------------------------------------------------------------------
// 7. 设备枚举
// ---------------------------------------------------------------------------
struct Device { int index; std::string name; DWORD flags; };

inline void EnumInputs(std::vector<Device>& out)
{
    out.clear();
    if (!g_loaded) return;
    BASS_DEVICEINFO di;
    for (int i = 0; pRecGetDevInfo((DWORD)i, &di); ++i)
        if (di.flags & BASS_DEVICE_ENABLED)
            out.push_back({ i, di.name ? di.name : "(未命名)", di.flags });
}

inline void EnumOutputs(std::vector<Device>& out)
{
    out.clear();
    if (!g_loaded) return;
    BASS_DEVICEINFO di;
    for (int i = 0; pGetDevInfo((DWORD)i, &di); ++i)
        if (di.flags & BASS_DEVICE_ENABLED)
            out.push_back({ i, di.name ? di.name : "(未命名)", di.flags });
}

// ---------------------------------------------------------------------------
// 8. 启动 / 停止
// ---------------------------------------------------------------------------
// BASS_Init 的 freq 不能给 0（实测 err=6 BASS_ERROR_FORMAT），按 48000/44100/-1 依次试
inline bool InitOutput(int dev)
{
    static const DWORD kRates[3] = { 48000, 44100, (DWORD)-1 };
    for (int i = 0; i < 3; ++i)
        if (pInit(dev < 0 ? -1 : dev, kRates[i], 0, nullptr, nullptr)) return true;
    return false;
}

inline bool Start(int inDev /*-1=默认*/, int outDev /*-1=默认*/)
{
    if (!g_loaded) { g_err = "BASS 未加载"; return false; }
    if (g_running) return true;
    g_err.clear();

    // 输出：BASS_Init(device, freq, flags, win, clsid)
    if (!InitOutput(outDev))
    {
        SetErr("BASS_Init 输出初始化失败");
        return false;
    }

    // 采集：BASS_RecordInit(device)
    if (!pRecInit(inDev))     // -1 = 默认采集设备
    {
        SetErr("BASS_RecordInit 失败（虚拟声卡未就绪？）");
        pFree();
        return false;
    }
    if (inDev >= 0) pRecSetDevice((DWORD)inDev);

    // 先按 16bit 开采集，失败再试 float；freq=0 => 用设备原生率
    g_isFloat = false;
    g_rec = pRecStart(0, 2, 0, &RecProc, nullptr);
    if (!g_rec)
    {
        g_isFloat = true;
        g_rec = pRecStart(0, 2, BASS_SAMPLE_FLOAT, &RecProc, nullptr);
    }
    if (!g_rec)
    {
        SetErr("BASS_RecordStart 失败");
        pFree();
        return false;
    }

    // 取实际采样率/声道，建同格式推流（免重采样）
    BASS_CHANNELINFO ci{};
    g_rate = 48000; g_chans = 2;
    if (pChanInfo(g_rec, &ci)) { g_rate = ci.freq; g_chans = ci.chans; }

    // BASS_StreamCreate(freq, chans, flags, proc, user)
    //   flags=0；proc = (STREAMPROC*)-1 即 STREAMPROC_PUSH（推流模式）
    g_out = pStreamCreate(g_rate, g_chans, 0, (STREAMPROC*)(INT_PTR)-1, nullptr);
    if (!g_out)
    {
        SetErr("BASS_StreamCreate 失败");
        pStop(g_rec); g_rec = 0; pFree();
        return false;
    }
    pPlay(g_out, FALSE);

    g_running = true;
    return true;
}

inline void StopAll()
{
    if (!g_loaded) return;
    if (g_out) { pStop(g_out); g_out = 0; }
    if (g_rec) { pStop(g_rec); g_rec = 0; }
    pRecFree(); pFree();
    g_running = false;
    g_meterRms.store(0);
    g_meterPeak.store(0);
}

// ---------------------------------------------------------------------------
// 9. 对外小接口
// ---------------------------------------------------------------------------
inline void SetLevelIndex(int lv)
{
    if (lv < 1) lv = 1;
    if (lv > 10) lv = 10;
    g_levelIdx.store(lv);
    g_gain.store(LevelToGain(lv));
}
inline int   LevelIndex() { return g_levelIdx.load(); }
inline float GainX()      { return g_gain.load(); }
inline void  SetMonitor(bool on) { g_monitor.store(on); }
inline bool  Monitor()    { return g_monitor.load(); }
inline void  SetMode(int m) { g_mode.store(m ? 1 : 0); }
inline int   Mode()       { return g_mode.load(); }
inline bool  Loaded()     { return g_loaded; }
inline bool  Running()    { return g_running; }
inline unsigned ClipCount() { return g_clipCount.load(); }
inline void  ResetClip()  { g_clipCount.store(0); }

struct Meter { unsigned rms; unsigned peak; };
inline Meter ReadMeter() { return Meter{ g_meterRms.load(), g_meterPeak.load() }; }

inline const std::string& LastError() { return g_err; }

// ---------------------------------------------------------------------------
// 10. 无界面运行支持：配置文件 + 日志 + 自动挑虚拟声卡 + 自动启动
//     配置: %LOCALAPPDATA%\Mio\gain.ini      日志: %LOCALAPPDATA%\Mio\gain.log
// ---------------------------------------------------------------------------
inline std::wstring MioDataDir()
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return std::wstring();
    std::wstring dir = base; dir += L"\\Mio";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

inline void Log(const char* fmt, ...)
{
    std::wstring dir = MioDataDir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\gain.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// 内嵌 bass.dll 释放：优先"程序同目录"，失败退 %LOCALAPPDATA%\Mio\，再退 %TEMP%
// ---------------------------------------------------------------------------
inline bool FileMatchesBlob(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool same = false;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart == (LONGLONG)kBassDllSize)
    {
        same = true;
        unsigned int off = 0;
        unsigned char buf[65536];
        while (off < kBassDllSize)
        {
            DWORD rd = kBassDllSize - off;
            if (rd > sizeof(buf)) rd = (DWORD)sizeof(buf);
            DWORD got = 0;
            if (!ReadFile(h, buf, rd, &got, nullptr) || got != rd) { same = false; break; }
            if (memcmp(buf, kBassDllBlob + off, rd) != 0) { same = false; break; }
            off += rd;
        }
    }
    CloseHandle(h);
    return same;
}

inline bool WriteBlobToFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    unsigned int off = 0;
    while (off < kBassDllSize)
    {
        DWORD wr = kBassDllSize - off;
        if (wr > (1u << 20)) wr = 1u << 20;
        DWORD wrote = 0;
        if (!WriteFile(h, kBassDllBlob + off, wr, &wrote, nullptr) || wrote != wr) { ok = false; break; }
        off += wr;
    }
    CloseHandle(h);
    if (ok && !FileMatchesBlob(path)) ok = false;   // 写完校验，防止半截文件
    return ok;
}

inline std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::wstring();
    std::wstring s(buf, n);
    const size_t sl = s.find_last_of(L"\\/");
    return (sl == std::wstring::npos) ? std::wstring() : s.substr(0, sl);
}

// 释放内嵌 bass.dll，返回可用路径（空 = 全部失败）
inline std::wstring ExtractBass()
{
    const std::wstring dir = ExeDir();
    if (!dir.empty())                                  // ① 程序同目录（需求指定）
    {
        const std::wstring beside = dir + L"\\bass.dll";
        if (FileMatchesBlob(beside)) return beside;    // 已存在且一致
        if (WriteBlobToFile(beside)) { Log("gain: 释放内嵌 bass.dll -> 程序目录 %ls", beside.c_str()); return beside; }
    }
    const std::wstring md = MioDataDir();              // ② 数据目录
    if (!md.empty())
    {
        const std::wstring q = md + L"\\bass.dll";
        if (FileMatchesBlob(q)) return q;
        if (WriteBlobToFile(q)) { Log("gain: 释放内嵌 bass.dll -> %ls", q.c_str()); return q; }
    }
    wchar_t tmp[MAX_PATH] = {};                        // ③ 临时目录
    if (GetTempPathW(MAX_PATH, tmp))
    {
        std::wstring q = tmp; q += L"Mio_bass.dll";
        if (FileMatchesBlob(q)) return q;
        if (WriteBlobToFile(q)) { Log("gain: 释放内嵌 bass.dll -> %ls", q.c_str()); return q; }
    }
    return std::wstring();
}


struct Config
{
    int  enable   = 1;      // 1 = 开机自动启动增益链路
    int  level    = 10;     // 工作级别 1..10（默认最大档 4.5x）
    int  mode     = 1;      // 0=Mode1 线性硬限幅  1=Mode2 软限幅(tanh)
    int  monitor  = 1;      // 1 = 把处理后的声音送到输出
    int  input    = -2;     // -2=自动挑虚拟声卡  -1=系统默认采集  >=0=BASS 采集设备索引
    int  output   = -1;     // -1=系统默认输出     >=0=BASS 输出设备索引
    float volume  = 1.0f;   // 回放流音量 0..1
};

inline std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline Config LoadConfig()
{
    Config c;
    std::wstring dir = MioDataDir();
    if (dir.empty()) return c;
    std::wstring path = dir + L"\\gain.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f)
    {
        // 首次运行：落一份带注释的默认配置，方便手改
        FILE* w = nullptr;
        if (_wfopen_s(&w, path.c_str(), L"w") == 0 && w)
        {
            fputs("; Mio 放音增益配置（无界面版）\n"
                  "; enable  : 1=开机自动启动增益链路, 0=不启动\n"
                  "; level   : 工作级别 1..10  -> 倍率 0.5/0.75/1/1.5/2/2.5/3/3.5/4/4.5  (默认10=最大4.5x)\n"
                  "; mode    : 0=线性+硬限幅, 1=线性+tanh软限幅\n"
                  "; monitor : 1=把处理后的声音送到输出设备\n"
                  "; input   : -2=自动挑虚拟声卡, -1=系统默认采集, >=0=采集设备索引\n"
                  "; output  : -1=系统默认输出, >=0=输出设备索引\n"
                  "; volume  : 回放流音量 0.0~1.0\n"
                  "enable=1\nlevel=10\nmode=1\nmonitor=1\ninput=-2\noutput=-1\nvolume=1.0\n", w);
            fclose(w);
        }
        return c;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        const char* pch = line;
        while (*pch == ' ' || *pch == '\t') ++pch;
        if (*pch == ';' || *pch == '#' || *pch == '[' || *pch == 0 || *pch == '\n' || *pch == '\r') continue;
        char key[64] = {}, val[128] = {};
        if (sscanf_s(pch, "%63[^= \t]=%127[^\r\n]", key, (unsigned)sizeof(key), val, (unsigned)sizeof(val)) != 2) continue;
        std::string k = Trim(key), v = Trim(val);
        if      (k == "enable")  c.enable  = atoi(v.c_str());
        else if (k == "level")   c.level   = atoi(v.c_str());
        else if (k == "mode")    c.mode    = atoi(v.c_str());
        else if (k == "monitor") c.monitor = atoi(v.c_str());
        else if (k == "input")   c.input   = atoi(v.c_str());
        else if (k == "output")  c.output  = atoi(v.c_str());
        else if (k == "volume")  c.volume  = (float)atof(v.c_str());
    }
    fclose(f);
    return c;
}

// 名字像"虚拟声卡/环回"的设备优先（TT语音 的输出桥接就落在这类设备上）
inline bool LooksVirtual(const std::string& n)
{
    static const char* kPat[] = { "CABLE", "VB-Audio", "Voicemeeter", "虚拟", "Virtual",
                                  "Loopback", "Stereo Mix", "立体声混音", "What U Hear" };
    for (const char* p : kPat)
    {
        // 大小写不敏感子串匹配
        std::string a = n, b = p;
        for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : b) ch = (char)tolower((unsigned char)ch);
        if (a.find(b) != std::string::npos) return true;
    }
    return false;
}

inline std::string CutDevName(std::string v)
{
    if (v.rfind("LOOPBACK ", 0) == 0) v = v.substr(9);
    size_t b = v.find(" (");
    if (b != std::string::npos) v = v.substr(0, b);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

inline bool AutoStart()
{
    const Config cfg = LoadConfig();
    if (!cfg.enable)
    {
        Log("gain: enable=0, 跳过");
        return false;
    }

    // 内嵌 bass.dll：释放到程序同目录（退 Mio 数据目录 / TEMP）后加载
    const std::wstring dll = ExtractBass();
    if (dll.empty())
    {
        Log("gain: 释放内嵌 bass.dll 失败（程序目录 / Mio 数据目录 / TEMP 都不可写）");
        return false;
    }
    if (!Load(dll.c_str()))
    {
        Log("gain: 加载 bass.dll 失败: %s", LastError().c_str());
        return false;
    }

    // 先 BASS_Init(输出)，拿到设备列表
    if (!InitOutput(cfg.output))
    {
        Log("gain: BASS_Init 失败 (err=%d)", pErr ? pErr() : -1);
        return false;
    }

    // 枚举（采集列表要在 Init 之后）
    std::vector<Device> outs, ins;
    EnumOutputs(outs);
    EnumInputs(ins);
    Log("gain: 输出设备 %d 个:", (int)outs.size());
    for (auto& d : outs) Log("   [%d]%s %s", d.index, (d.flags & BASS_DEVICE_DEFAULT) ? "*" : " ", d.name.c_str());
    Log("gain: 采集设备 %d 个:", (int)ins.size());
    for (auto& d : ins)  Log("   [%d]%s %s", d.index, (d.flags & BASS_DEVICE_LOOPBACK) ? " LOOPBACK" : "        ", d.name.c_str());

    int inDev = cfg.input;
    if (inDev == -2)
    {
        inDev = -1;
        // (a) 优先"虚拟声卡"（最干净：TT语音 输出指到它，物理扬声器不会再听到原声）
        for (auto& d : ins)
            if (LooksVirtual(d.name)) { inDev = d.index; Log("gain: 自动选中虚拟声卡 [%d] %s", d.index, d.name.c_str()); break; }
        // (b) 其次用 BASS 的环回设备；跳过"与播放设备同一路"的（那必然正反馈）
        if (inDev < 0)
        {
            std::string outName;
            for (auto& d : outs) if (d.index == cfg.output || (cfg.output < 0 && (d.flags & BASS_DEVICE_DEFAULT))) outName = CutDevName(d.name);
            for (auto& d : ins)
            {
                if (!(d.flags & BASS_DEVICE_LOOPBACK)) continue;
                if (!outName.empty() && CutDevName(d.name) == outName)
                {
                    Log("gain: 跳过环回 [%d] %s（它就是本次输出那一路，采它会正反馈）", d.index, d.name.c_str());
                    continue;
                }
                inDev = d.index;
                Log("gain: 自动选中环回设备 [%d] %s", d.index, d.name.c_str());
                break;
            }
        }
        if (inDev < 0)
        {
            Log("gain: 没找到虚拟声卡/环回采集设备 —— 为避免麦克风->扬声器啸叫，不自动启动。");
            Log("gain: 请在 gain.ini 里把 input 手动设成上面列出的某个索引后重启 Mio。");
            pFree();
            return false;
        }
    }

    // 自激硬保护：环回采集的设备就是本次回放的那一路 -> 必然正反馈，直接拒绝
    {
        std::string inName, outName;
        for (auto& d : ins)  if (d.index == inDev) inName = d.name;
        for (auto& d : outs) if (d.index == cfg.output || (cfg.output < 0 && (d.flags & BASS_DEVICE_DEFAULT))) outName = d.name;
        const std::string a = CutDevName(inName), b2 = CutDevName(outName);
        if (!a.empty() && a == b2)
        {
            Log("gain: [拒绝启动] 采集设备 \"%s\" 与输出设备 \"%s\" 是同一路，会形成正反馈爆音。", inName.c_str(), outName.c_str());
            Log("gain: 请把 gain.ini 的 output 改成另一个设备（例如物理耳机/扬声器那一路），或装 VB-CABLE 走虚拟声卡。");
            pFree();
            return false;
        }

        bool loop = false;
        for (auto& d : ins) if (d.index == inDev && (d.flags & BASS_DEVICE_LOOPBACK)) loop = true;
        if (loop)
            Log("gain: [警告] 用的是环回采集。若被放大的声音又回到本次输出设备，会形成正反馈(爆音)；"
                "已内置自激保护会自动切断。最干净的做法：装 VB-CABLE，把 TT语音 输出指到它，再让 Mio 采它。");
    }

    // 枚举用的那次 BASS_Init 必须先释放，否则 Start 里再 Init 会 err=14(BASS_ERROR_ALREADY)
    pFree();

    SetLevelIndex(cfg.level);
    SetMode(cfg.mode);
    SetMonitor(cfg.monitor != 0);
    if (!Start(inDev, cfg.output))
    {
        Log("gain: Start 失败: %s", LastError().c_str());
        return false;
    }
    if (g_out && cfg.volume >= 0.0f && cfg.volume <= 1.0f)
        pSetAttr(g_out, BASS_ATTRIB_VOL, cfg.volume);

    Log("gain: 已启动  level=%d/10(%.2fx)%s mode=%d monitor=%d in=%d out=%d rate=%u ch=%u fmt=%s",
        LevelIndex(), GainX(), (LevelIndex() >= 10 ? " [最大]" : ""), Mode(), Monitor() ? 1 : 0, inDev, cfg.output,
        (unsigned)g_rate, (unsigned)g_chans, g_isFloat ? "f32" : "s16");
    return true;
}

} // namespace mio_gain
