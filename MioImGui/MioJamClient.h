#pragma once
// ---------------------------------------------------------------------------
// MioJamClient.h —— Mio 侧的"干扰"客户端
//   1. 管理共享内存 Local\MioJammer_Ctl_v1（创建/打开 + 读写控制块）
//   2. 把 MioJammer.dll 注入到 TT语音 进程（复用 mio_zego 的注入与目标枚举）
//   3. 异步注入 + 注入结果统计，供 UI 轮询
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>

#include "MioJamShared.h"
#include "ZegoProbeClient.h"   // 复用 mio_zego::CollectTargets / InjectProbe / GetOwnDir

namespace mio_jam {

// ---------------------------------------------------------------------------
// DLL 路径：与 MioImGui.exe 同目录
// ---------------------------------------------------------------------------
inline const std::wstring& DllPath()
{
    static std::wstring cached;
    if (!cached.empty())
        return cached;
    std::wstring dir;
    if (mio_zego::GetOwnDir(dir))
        cached = dir + L"\\MioJammer.dll";
    return cached;
}

// ---------------------------------------------------------------------------
// 共享内存
// ---------------------------------------------------------------------------
inline MioJamShared*& View()
{
    static MioJamShared* v = nullptr;
    return v;
}
inline HANDLE& MapHandle()
{
    static HANDLE h = nullptr;
    return h;
}

inline bool EnsureMapping()
{
    if (View())
        return true;
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(MioJamShared), MIO_JAM_SHARED_NAME);
    if (!m)
        m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_JAM_SHARED_NAME);
    if (!m)
        return false;
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioJamShared));
    if (!view)
    {
        CloseHandle(m);
        return false;
    }
    MapHandle() = m;
    View() = (MioJamShared*)view;
    // Mio 侧只保证块存在并打上自己的标记：魔数已由载荷或本进程写过就不动内容
    if (View()->magic != MIO_JAM_MAGIC)
    {
        memset(View(), 0, sizeof(MioJamShared));
        View()->magic = MIO_JAM_MAGIC;
        View()->jamSwitch = 0;          // 默认关，和面板复选框一致
        View()->payloadType = 0;          // 0 = 不做 PT 过滤
        View()->mode = MIO_JAM_MODE_AUTO;
    }
    return true;
}

inline bool Ready()
{
    return EnsureMapping() && View() && View()->magic == MIO_JAM_MAGIC;
}

// ---------------------------------------------------------------------------
// 读状态快照
// ---------------------------------------------------------------------------
struct Snapshot
{
    bool     ok = false;
    uint32_t jamEnabled = 0;
    uint32_t mode = 0;
    uint32_t payloadType = 0;
    uint32_t pid = 0;
    uint32_t initialized = 0;
    std::wstring exe;
};

inline bool ReadSnap(Snapshot& s)
{
    if (!Ready())
    {
        s.ok = false;
        return false;
    }
    MioJamShared* v = View();
    s.ok = true;
    s.jamEnabled  = v->jamEnabled;
    s.mode        = v->mode;
    s.payloadType = v->payloadType;
    s.pid         = v->pid;
    s.initialized = v->initialized;
    wchar_t buf[0x3F + 1] = {};
    wcsncpy(buf, v->exeName, 0x3F);
    s.exe = buf;
    return true;
}

// ---------------------------------------------------------------------------
// 写控制（广播到所有已注入进程）
// ---------------------------------------------------------------------------
inline void SetEnabled(bool on)
{
    if (!Ready()) return;
    View()->jamSwitch = on ? 1u : 0u;
}
inline void SetMode(int m)
{
    if (!Ready()) return;
    View()->mode = (uint32_t)m;
}
inline void SetPayloadType(int pt)
{
    if (!Ready()) return;
    View()->payloadType = (uint32_t)pt;
}
inline void RequestUnload()
{
    if (!Ready()) return;
    View()->reqUnload = 1;
}

// ---------------------------------------------------------------------------
// 注入：记录已注入 PID（跨帧保留，进程死了自动剔除）
// ---------------------------------------------------------------------------
inline std::vector<DWORD>& Injected()
{
    static std::vector<DWORD> v;
    return v;
}
inline std::mutex& InjectedMutex()
{
    static std::mutex m;
    return m;
}
inline std::atomic<bool>& Busy()
{
    static std::atomic<bool> b(false);
    return b;
}

inline int LiveInjectedCount()
{
    std::lock_guard<std::mutex> g(InjectedMutex());
    auto& v = Injected();
    int n = 0;
    for (DWORD pid : v)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) { CloseHandle(h); ++n; }
    }
    return n;
}

inline void PruneDead()
{
    std::lock_guard<std::mutex> g(InjectedMutex());
    auto& v = Injected();
    for (size_t i = 0; i < v.size();)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, v[i]);
        if (h) { CloseHandle(h); ++i; }
        else   { v.erase(v.begin() + i); }
    }
}

// 同步注入一个 PID（内部用）；调用方保证不在 UI 线程长时间阻塞
inline bool InjectOne(DWORD pid, std::wstring& err)
{
    const std::wstring& dll = DllPath();
    if (dll.empty())
    {
        err = L"找不到 MioJammer.dll 路径";
        return false;
    }
    return mio_zego::InjectProbe(pid, dll, err);
}

// 异步"注入全部"：枚举 TT语音 进程 -> 逐个注入 -> 记录 PID
inline void InjectAllAsync()
{
    if (Busy().exchange(true))
        return;
    std::thread([]()
    {
        std::vector<mio_zego::TargetProcess> targets;
        mio_zego::CollectTargets(targets);
        int ok = 0, skip = 0, fail = 0;
        for (const auto& t : targets)
        {
            if (!t.injectable && !t.isRoomRenderer)
                continue;
            {
                std::lock_guard<std::mutex> g(InjectedMutex());
                auto& v = Injected();
                bool already = false;
                for (DWORD p : v) if (p == t.pid) { already = true; break; }
                if (already) { ++skip; continue; }
            }
            std::wstring err;
            if (InjectOne(t.pid, err))
            {
                std::lock_guard<std::mutex> g(InjectedMutex());
                Injected().push_back(t.pid);
                ++ok;
            }
            else
            {
                ++fail;
            }
        }
        (void)ok; (void)skip; (void)fail;
        Busy() = false;
    }).detach();
}

// ---------------------------------------------------------------------------
// 统计：汇总所有已注入进程的拦截计数
// ---------------------------------------------------------------------------
struct StatsTotals
{
    unsigned long long seen = 0, rtp = 0, blind = 0, dropped = 0;
    int procs = 0;
};

inline MioJamStats*& StatView()
{
    static MioJamStats* v = nullptr;
    return v;
}

inline MioJamStats* EnsureStats()
{
    if (StatView()) return StatView();
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(MioJamStats), MIO_JAM_STATS_NAME);
    if (!m) m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_JAM_STATS_NAME);
    if (!m) return nullptr;
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioJamStats));
    if (!view) { CloseHandle(m); return nullptr; }
    StatView() = (MioJamStats*)view;
    if (StatView()->magic != MIO_JAM_STATS_MAGIC)
    {
        memset(StatView(), 0, sizeof(MioJamStats));
        StatView()->magic = MIO_JAM_STATS_MAGIC;
    }
    return StatView();
}

inline StatsTotals ReadStats()
{
    StatsTotals t;
    MioJamStats* st = EnsureStats();
    if (!st || st->magic != MIO_JAM_STATS_MAGIC) return t;
    for (int i = 0; i < MIO_JAM_STAT_SLOTS; ++i)
    {
        const MioJamStatSlot& s = st->slots[i];
        if (!s.pid) continue;
        t.seen += s.seen; t.rtp += s.rtp; t.blind += s.blind; t.dropped += s.dropped;
        ++t.procs;
    }
    return t;
}

inline void ResetStats()
{
    MioJamStats* st = EnsureStats();
    if (!st) return;
    for (int i = 0; i < MIO_JAM_STAT_SLOTS; ++i)
    {
        st->slots[i].seen = st->slots[i].rtp = st->slots[i].blind = st->slots[i].dropped = 0;
    }
}

} // namespace mio_jam
