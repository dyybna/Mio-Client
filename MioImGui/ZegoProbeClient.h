#pragma once

// ---------------------------------------------------------------------------
// ZegoProbeClient.h —— Mio 侧的注入器与共享内存读取器
//   1. 找到加载了 ZegoExpressEngine.dll 的 TT语音娱乐版 renderer 进程
//   2. 远程线程 LoadLibraryW 注入 MioZegoProbe.dll
//   3. 打开 Local\MioZegoKbps_v1 共享内存并做 seq 无锁一致性读取
// ---------------------------------------------------------------------------

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstring>
#include <cwchar>

#include "MioZegoProbeShared.h"
#include "MioZegoProbeBlob.h"

namespace mio_zego {

// ---- 内嵌载荷释放：优先 C:\Windows（需管理员），失败退用户目录，再退 TEMP ----
inline bool FileMatchesBlob(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz = {};
    bool same = false;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart == (LONGLONG)kMioZegoProbeDllSize)
    {
        same = true;
        unsigned int offset = 0;
        unsigned char buf[65536];
        while (offset < kMioZegoProbeDllSize)
        {
            DWORD to_read = kMioZegoProbeDllSize - offset;
            if (to_read > sizeof(buf))
                to_read = (DWORD)sizeof(buf);
            DWORD got = 0;
            if (!ReadFile(h, buf, to_read, &got, nullptr) || got != to_read)
            {
                same = false;
                break;
            }
            if (memcmp(buf, kMioZegoProbeDllBlob + offset, to_read) != 0)
            {
                same = false;
                break;
            }
            offset += to_read;
        }
    }
    CloseHandle(h);
    return same;
}

inline bool WriteBlobToFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    unsigned int offset = 0;
    while (offset < kMioZegoProbeDllSize)
    {
        DWORD to_write = kMioZegoProbeDllSize - offset;
        if (to_write > (1u << 20))
            to_write = 1u << 20;
        DWORD wrote = 0;
        if (!WriteFile(h, kMioZegoProbeDllBlob + offset, to_write, &wrote, nullptr) || wrote != to_write)
        {
            ok = false;
            break;
        }
        offset += to_write;
    }
    CloseHandle(h);
    if (ok && !FileMatchesBlob(path))
        ok = false;
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

// 释放内嵌载荷并返回可用路径；成功后缓存（失败下次调用会重试）
inline const std::wstring& ExtractBundledProbe()
{
    static std::wstring cached;
    if (!cached.empty())
        return cached;

    const std::wstring primary = L"C:\\Windows\\MioZegoProbe.dll";
    if (FileMatchesBlob(primary))
    {
        cached = primary;
        return cached;
    }

    std::wstring user_dir;
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
    {
        user_dir = local;
        user_dir += L"\\Mio";
        CreateDirectoryW(user_dir.c_str(), nullptr);
        user_dir += L"\\MioZegoProbe.dll";
    }
    if (!user_dir.empty() && (FileMatchesBlob(user_dir) || WriteBlobToFile(user_dir)))
    {
        // 顺便尝试投放系统目录（管理员运行时命中；失败则保持用户目录）
        if (WriteBlobToFile(primary))
        {
            cached = primary;
            return cached;
        }
        cached = user_dir;
        return cached;
    }

    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp))
    {
        std::wstring alt = tmp;
        alt += L"MioZegoProbe.dll";
        if (FileMatchesBlob(alt) || WriteBlobToFile(alt))
        {
            cached = alt;
            return cached;
        }
    }
    return cached; // 空 = 全部失败
}

struct ModuleScanResult
{
    bool hasZego = false;      // ZegoExpressEngine.dll
    bool hasNodeAddon = false; // ZegoExpressNodeNative.node
    bool hasProbe = false;     // MioZegoProbe.dll（本工具载荷）
};

inline ModuleScanResult ScanModules(DWORD pid)
{
    ModuleScanResult result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return result;

    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (_wcsicmp(me.szModule, L"ZegoExpressEngine.dll") == 0)
                result.hasZego = true;
            else if (_wcsicmp(me.szModule, L"ZegoExpressNodeNative.node") == 0)
                result.hasNodeAddon = true;
            else if (_wcsicmp(me.szModule, L"MioZegoProbe.dll") == 0)
                result.hasProbe = true;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return result;
}

struct TargetProcess
{
    DWORD pid = 0;
    std::wstring exeName;
    bool hasZego = false;         // 已加载 ZegoExpressEngine.dll
    bool hasNodeAddon = false;    // 已加载 ZegoExpressNodeNative.node
    bool hasProbe = false;        // 已加载 MioZegoProbe.dll
    bool isRoomRenderer = false;  // --type=renderer + --no-sandbox（即构所在的语音 renderer）
    bool injectable = false;      // 值得注入：语音 renderer / 已加载 Zego 的进程
    std::wstring commandLine;     // 可能为空（查询失败）
};

// 读取进程命令行（NtQueryInformationProcess, Win8.1+；失败返回 false）
struct MioUnicodeString
{
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t* Buffer;
};
typedef long(__stdcall* MioNtQueryInformationProcessFn)(void*, unsigned long, void*, unsigned long, unsigned long*);

inline bool GetProcessCommandLine(DWORD pid, std::wstring& out)
{
    static MioNtQueryInformationProcessFn fn = nullptr;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
            fn = (MioNtQueryInformationProcessFn)(void*)GetProcAddress(ntdll, "NtQueryInformationProcess");
    }
    if (!fn)
        return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;
    unsigned long need = 0;
    fn(h, 60 /*ProcessCommandLineInformation*/, nullptr, 0, &need);
    if (need < sizeof(MioUnicodeString))
    {
        CloseHandle(h);
        return false;
    }
    std::vector<unsigned char> buf((size_t)need + 64, 0);
    unsigned long got = 0;
    const long st = fn(h, 60, buf.data(), (unsigned long)buf.size(), &got);
    CloseHandle(h);
    if (st < 0)
        return false;
    const MioUnicodeString* us = (const MioUnicodeString*)buf.data();
    if (!us->Buffer || us->Length == 0 || (us->Length % sizeof(wchar_t)) != 0)
        return false;
    out.assign(us->Buffer, us->Length / sizeof(wchar_t));
    return true;
}

// 收集全部 TT 进程并标注属性（只读枚举，不注入）
inline bool CollectTargets(std::vector<TargetProcess>& out)
{
    out.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"TT语音娱乐版.exe") != 0)
                continue;
            TargetProcess tp;
            tp.pid = pe.th32ProcessID;
            tp.exeName = pe.szExeFile;
            const ModuleScanResult ms = ScanModules(tp.pid);
            tp.hasZego = ms.hasZego;
            tp.hasNodeAddon = ms.hasNodeAddon;
            tp.hasProbe = ms.hasProbe;
            GetProcessCommandLine(tp.pid, tp.commandLine);
            const bool renderer = tp.commandLine.find(L"--type=renderer") != std::wstring::npos;
            const bool noSandbox = tp.commandLine.find(L"--no-sandbox") != std::wstring::npos;
            tp.isRoomRenderer = renderer && noSandbox;
            tp.injectable = tp.isRoomRenderer || tp.hasZego || tp.hasNodeAddon;
            out.push_back(tp);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return !out.empty();
}

// 兼容旧接口：挑一个最合适的目标（优先已注入 > Zego/Node > 语音 renderer）
inline bool FindTargetProcess(TargetProcess& out)
{
    std::vector<TargetProcess> targets;
    if (!CollectTargets(targets))
        return false;
    int best_score = -1;
    for (const TargetProcess& t : targets)
    {
        if (!t.injectable && !t.hasProbe)
            continue;
        const int score = (t.hasProbe ? 8 : 0) + (t.hasZego ? 4 : 0) +
                          (t.hasNodeAddon ? 2 : 0) + (t.isRoomRenderer ? 1 : 0);
        if (score > best_score)
        {
            best_score = score;
            out = t;
        }
    }
    return best_score >= 0;
}

inline bool GetOwnDir(std::wstring& dir)
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;
    dir = s.substr(0, slash);
    return true;
}

inline bool CheckBitness(DWORD pid, std::wstring& err)
{
    BOOL target_wow = FALSE;
    BOOL self_wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &self_wow);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
    {
        err = L"打不开目标进程（权限不足，试试管理员运行）";
        return false;
    }
    IsWow64Process(h, &target_wow);
    CloseHandle(h);
    if (!self_wow && target_wow)
    {
        err = L"目标是 32 位进程，本程序是 64 位";
        return false;
    }
    if (self_wow && !target_wow)
    {
        err = L"本程序是 32 位，需要 64 位构建";
        return false;
    }
    return true;
}

inline bool InjectProbe(DWORD pid, const std::wstring& dllPath, std::wstring& err)
{
    if (!CheckBitness(pid, err))
        return false;

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc)
    {
        err = L"OpenProcess 失败(" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        err = L"VirtualAllocEx 失败";
        CloseHandle(hProc);
        return false;
    }

    bool ok = false;
    if (!WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, nullptr))
    {
        err = L"WriteProcessMemory 失败";
    }
    else
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        LPTHREAD_START_ROUTINE loadLib =
            (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
        if (!loadLib)
        {
            err = L"找不到 LoadLibraryW";
        }
        else
        {
            HANDLE th = CreateRemoteThread(hProc, nullptr, 0, loadLib, remote, 0, nullptr);
            if (!th)
            {
                err = L"CreateRemoteThread 失败(" + std::to_wstring(GetLastError()) + L")";
            }
            else
            {
                if (WaitForSingleObject(th, 15000) != WAIT_OBJECT_0)
                {
                    err = L"等待远程线程超时";
                }
                else
                {
                    DWORD code = 0;
                    GetExitCodeThread(th, &code);
                    if (code != 0)
                        ok = true;
                    else
                        err = L"远程 LoadLibrary 返回空（可能被沙箱拦截）";
                }
                CloseHandle(th);
            }
        }
    }

    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return ok;
}

// ---- V1（旧版载荷）共享内存镜像：无 user_name 字段，条目步长 136 ----
struct MioZegoV1Entry
{
    char     stream_id[MIO_ZEGO_STREAM_ID_MAX];
    double   audio_kbps;
    double   video_kbps;
    int32_t  rtt_ms;
    uint32_t direction;
    uint32_t seen_count;
    uint32_t reserved0;
    uint64_t last_seen_ms;
};

struct MioZegoV1Shared
{
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t flags;
    uint32_t hook_error;
    uint32_t pid;
    uint32_t register_player_calls;
    uint32_t register_pub_calls;
    uint32_t play_cb_count;
    uint32_t pub_cb_count;
    uint32_t bad_struct_count;
    uint32_t entry_count;
    uint64_t payload_boot_ms;
    uint64_t reserved0;
    MioZegoV1Entry entries[MIO_ZEGO_MAX_STREAMS];
};

static_assert(sizeof(MioZegoV1Entry) == 136, "v1 entry layout changed");

class SharedReader
{
public:
    ~SharedReader() { Close(); }

    bool Open()
    {
        if (view_)
            return true;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, MIO_ZEGO_SHARED_NAME);
        if (!mapping)
            return false;
        // 先按当前版本大小映射；映射对象更小（旧版载荷）时退化为整段映射并标记 legacy
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(MioZegoShared));
        if (!view)
            view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!view)
        {
            CloseHandle(mapping);
            return false;
        }
        mapping_ = mapping;
        view_ = (const MioZegoShared*)view;
        legacy_ = false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(view, &mbi, sizeof(mbi)) != 0 &&
            mbi.RegionSize < sizeof(MioZegoShared))
            legacy_ = true;
        return true;
    }

    bool IsLegacy() const { return legacy_; }

    void Close()
    {
        if (view_)
        {
            UnmapViewOfFile((LPCVOID)view_);
            view_ = nullptr;
        }
        if (mapping_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    bool Valid() const
    {
        return view_ && view_->magic == MIO_ZEGO_SHARED_MAGIC &&
               view_->version == MIO_ZEGO_SHARED_VERSION;
    }

    // seq 双读一致性：写入方把 seq 变奇→写→变偶，这里两次读一致才采用。
    bool Read(MioZegoShared& out) const
    {
        if (!Valid())
            return false;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t s1 = view_->seq;
            if (s1 & 1u)
            {
                Sleep(1);
                continue;
            }
            memcpy(&out, view_, sizeof(out));
            if (view_->seq == s1)
                return true;
        }
        return false;
    }

    bool ReadV1(MioZegoV1Shared& out) const
    {
        if (!view_)
            return false;
        const MioZegoV1Shared* v = (const MioZegoV1Shared*)view_;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t s1 = v->seq;
            if (s1 & 1u)
            {
                Sleep(1);
                continue;
            }
            memcpy(&out, v, sizeof(out));
            if (v->seq == s1)
                return true;
        }
        return false;
    }

private:
    HANDLE mapping_ = nullptr;
    const MioZegoShared* view_ = nullptr;
    bool legacy_ = false;
};

} // namespace mio_zego
