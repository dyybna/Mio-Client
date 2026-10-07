// ---------------------------------------------------------------------------
// MioJammer.dll —— Mio 的"干扰"载荷（注入 TT语音 客户端进程）
//
// 原理：内联钩住本进程 ws2_32 的 send / sendto / WSASend / WSASendTo，
//       把外发的 RTP 语音包载荷砸烂（FF FF 00 00 + 随机）、随机化 seq/timestamp、
//       并随机截断 —— 对端听到噪声/爆音/卡死。
// 控制：命名共享内存 Local\MioJammer_Ctl_v1（多进程共用一个块 = 天然广播通道）
// 热键：F8（注册失败自动退 Ctrl+F8）
//
// 说明：本载荷是 111\HookDll.dll 的行为重建 + 5 处加固，详见 HookDll_reconstructed.c。
//       与 MioZegoProbe.dll 互不干扰（各自静态链接一份 MinHook，钩的函数不重叠）。
// ---------------------------------------------------------------------------
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cstdarg>
#include <malloc.h>

#include "MinHook.h"
#include "MioJamShared.h"

// 统计块（让 Mio 能看见拦截情况）
static MioJamStatSlot* g_stat = 0;
static void stat_ensure()
{
    if (g_stat) return;
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(MioJamStats), MIO_JAM_STATS_NAME);
    if (!m) m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_JAM_STATS_NAME);
    if (!m) return;
    MioJamStats* st = (MioJamStats*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioJamStats));
    if (!st) { CloseHandle(m); return; }
    if (st->magic != MIO_JAM_STATS_MAGIC) { memset(st, 0, sizeof(*st)); st->magic = MIO_JAM_STATS_MAGIC; }
    DWORD me = GetCurrentProcessId();
    for (int i = 0; i < MIO_JAM_STAT_SLOTS; ++i)
    {
        if (st->slots[i].pid == me) { g_stat = &st->slots[i]; return; }
        if (st->slots[i].pid == 0) { st->slots[i].pid = me; st->slotCount++; g_stat = &st->slots[i]; return; }
    }
    g_stat = &st->slots[0];
}
#define STAT_INC(field) do { if (g_stat) InterlockedIncrement((volatile LONG*)&g_stat->field); } while (0)

extern "C" __declspec(dllexport) const char MioJammerBuildTag[] =
    "MioJammer v1 (hook-4 + rtp-hard + blind-relaxed)";

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static volatile LONG g_jamEnabled  = 0;   // 默认关闭：由 Mio 面板/热键开启（原外挂是注入即开）
static volatile LONG g_logEnabled  = 1;
static volatile LONG g_payloadType = 0;      // 0 = 不做 PT 过滤（加固项 1）
static void*         g_shmView     = 0;
static HANDLE        g_shmMap      = 0;
static volatile LONG g_mode        = MIO_JAM_MODE_AUTO;
static volatile LONG g_pktCounter  = 0;
static volatile LONG g_quit        = 0;
static DWORD         g_hotkeyTid   = 0;
static HINSTANCE     g_hinst       = 0;
static FILE*         g_logFile     = 0;
static FARPROC       g_orig_sendto = 0, g_orig_send = 0, g_orig_wsasend = 0, g_orig_wsasendto = 0;

typedef int (WINAPI *sendto_fn)(SOCKET, const char*, int, int, const struct sockaddr*, int);
typedef int (WINAPI *send_fn)(SOCKET, const char*, int, int);
typedef int (WINAPI *wsasend_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *wsasendto_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                   const struct sockaddr*, int,
                                   LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static sendto_fn    g_next_sendto = 0;
static send_fn      g_next_send = 0;
static wsasend_fn   g_next_wsasend = 0;
static wsasendto_fn g_next_wsasendto = 0;

// ---------------------------------------------------------------------------
// 日志  %LOCALAPPDATA%\Mio\jammer-<pid>.log（每个进程一份，便于多进程排查）
// ---------------------------------------------------------------------------
static void jam_log(const char* fmt, ...)
{
    va_list ap;
    if (!g_logEnabled) return;
    if (!g_logFile)
    {
        wchar_t base[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        wchar_t dir[MAX_PATH] = {};
        _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
        CreateDirectoryW(dir, nullptr);
        wchar_t path[MAX_PATH] = {};
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\jammer-%lu.log", base, (unsigned long)GetCurrentProcessId());
        if (_wfopen_s(&g_logFile, path, L"a") != 0) { g_logFile = nullptr; return; }
    }
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}

// ---------------------------------------------------------------------------
// 共享内存：写回状态
// ---------------------------------------------------------------------------
static void shm_publish()
{
    MioJamShared* c = (MioJamShared*)g_shmView;
    if (!c || c->magic != MIO_JAM_MAGIC) return;
    c->jamEnabled = (uint32_t)g_jamEnabled;
    c->logEnabled = (uint32_t)g_logEnabled;
    c->mode       = (uint32_t)g_mode;
    c->payloadType= (uint32_t)g_payloadType;
    c->zero20     = 0;
}

// ---------------------------------------------------------------------------
// 盲毁：非 RTP / 密文通道（加固项 5：不再死认 0xA6 0x04）
// ---------------------------------------------------------------------------
static int blind_destroy(SOCKET s, char* pkt, int len)
{
    int type = 0, tl = sizeof(type);
    BOOL udp = (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &tl) == 0) && (type == SOCK_DGRAM);
    if (len < 0x3C) return len;
    if (udp && len < 0x3C) return len;
    for (int i = 20; i < len; ++i) pkt[i] = (char)rand();
    if (g_stat) { InterlockedIncrement((volatile LONG*)&g_stat->blind); g_stat->lastMs = GetTickCount64(); }
    return len;
}

// ---------------------------------------------------------------------------
// 干扰核心：改包
// ---------------------------------------------------------------------------
static int jam_transform(SOCKET s, const char* proto, const struct sockaddr* to,
                         char* pkt, const char* orig, int len)
{
    int rtp_off = -1;

    // RTP 识别：只认 version==2（加固项 1：不再比对 payload type）
    if (orig && len >= 12)
    {
        unsigned char b0 = (unsigned char)orig[0];
        if ((b0 & 0xC0) == 0x80)
        {
            int off = 12 + ((b0 & 0x0F) * 4);
            if (b0 & 0x10)
            {
                if (off + 4 <= len)
                    off += 4 + ((((unsigned char)orig[off + 2] << 8) | (unsigned char)orig[off + 3]) * 4);
                else
                    off = len;
            }
            rtp_off = (off < len) ? off : -1;
        }
    }

    STAT_INC(seen);
    if (++g_pktCounter <= 200)
    {
        char hex[0x28 * 3 + 1];
        int n = (len < 0x28) ? len : 0x28;
        for (int i = 0; i < n; ++i) sprintf(hex + i * 3, "%02X ", (unsigned char)orig[i]);
        hex[n * 3] = 0;

        unsigned short port = 0;
        if (to)
        {
            if (((const struct sockaddr_in*)to)->sin_family == AF_INET)
                port = ntohs(((const struct sockaddr_in*)to)->sin_port);
        }
        else
        {
            struct sockaddr_in name; int nl = sizeof(name);
            if (getpeername(s, (struct sockaddr*)&name, &nl) == 0 && name.sin_family == AF_INET)
                port = ntohs(name.sin_port);
        }
        int type = 0, tl = sizeof(type); const char* kind = "?";
        if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &tl) == 0)
            kind = (type == SOCK_DGRAM) ? "UDP" : ((type == SOCK_STREAM) ? "TCP" : "?");
        jam_log("[%s/%s] PID=%lu port=%u len=%d rtp_off=%d : %s\n",
                proto, kind, GetCurrentProcessId(), port, len, rtp_off, hex);
    }

    // -----------------------------------------------------------------------
    // 【CUSTOM】TT语音 私有封装识别 —— 实测关键路径
    //   抓包实证：真实语音帧就是这种 15~40 字节的 UDP 包，
    //   结构 = [0..1] magic 20 21 | [2..3] seq | [4..] payload
    //   它既不是 RTP（首字节 0x20 → 版本位=0），也没有原外挂盲毁要的 A6 04 魔数，
    //   所以旧逻辑 100% 漏掉它（日志里全是 rtp_off=-1）。
    // -----------------------------------------------------------------------
    if (len >= 8 && (unsigned char)orig[0] == 0x20 && (unsigned char)orig[1] == 0x21)
    {
        pkt[2] = (char)rand();                                  // seq 打乱（保留 magic，否则对端直接丢包）
        pkt[3] = (char)rand();
        for (int i = 4; i < len; ++i) pkt[i] = (char)rand();     // 载荷全毁（含小包，不再有 60 字节门槛）
        STAT_INC(rtp);
        if (g_stat) g_stat->lastMs = GetTickCount64();
        return len;
    }

    // 「仅RTP」模式：非 RTP 一律放行
    if (g_mode == MIO_JAM_MODE_RTP && rtp_off < 0) return len;

    // 盲毁通道
    if (g_mode == MIO_JAM_MODE_BLIND || g_mode == MIO_JAM_MODE_CRASH3 || rtp_off < 0)
    {
        return blind_destroy(s, pkt, len);
    }

    // 砸 RTP
    {
        // 加固项 2：seq/timestamp(2..7) 一起随机；故意保留 SSRC(8..11)，
        //           否则对端会判为陌生流直接丢弃，反而减轻伤害
        if (len >= 8) for (int i = 2; i < 8; ++i) pkt[i] = (char)rand();

        int avail = len - rtp_off;
        if (avail <= 0) return len;
        pkt[rtp_off] = (char)0xFF;
        if (avail > 1)
        {
            pkt[rtp_off + 1] = (char)0xFF;
            if (avail >= 3)
            {
                pkt[rtp_off + 2] = 0x00;
                if (avail > 3) pkt[rtp_off + 3] = 0x00;
            }
        }
        for (int i = rtp_off + 4; i < len; ++i) pkt[i] = (char)rand();

        // 加固项 3：无条件截断，并夹紧到 len（不越界）
        STAT_INC(rtp);
        if (g_stat) g_stat->lastMs = GetTickCount64();
        int m = (avail < 0x28) ? avail : 0x28;
        int cut = (rtp_off + 4) + (rand() % m);
        if (cut > len) cut = len;
        return cut;
    }
}

// ---------------------------------------------------------------------------
// 四个网络钩子
// ---------------------------------------------------------------------------
static int WINAPI hook_sendto(SOCKET s, const char* buf, int len, int flags,
                              const struct sockaddr* to, int tolen)
{
    if (len < 8 || !buf || !g_jamEnabled)
        return g_next_sendto(s, buf, len, flags, to, tolen);

    if (g_mode == MIO_JAM_MODE_CRASH4)
    {
        int type = 0, tl = sizeof(type);
        BOOL tcp_long = (getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &tl) == 0)
                        && (type != SOCK_DGRAM) && (len >= 0x18);
        if (!tcp_long && ((unsigned char)buf[0] == 0x20 || (unsigned char)buf[0] == 0x2A))
            return len;   // 静默丢弃
    }

    char* copy = (char*)_alloca((len + 15) & ~15);
    memcpy(copy, buf, len);
    int n2 = jam_transform(s, "sendto", to, copy, buf, len);
    if (n2 < 0) return g_next_sendto(s, buf, len, flags, to, tolen);
    return g_next_sendto(s, copy, n2, flags, to, tolen);
}

static int WINAPI hook_send(SOCKET s, const char* buf, int len, int flags)
{
    if (len < 8 || !buf || !g_jamEnabled)
        return g_next_send(s, buf, len, flags);
    char* copy = (char*)_alloca((len + 15) & ~15);
    memcpy(copy, buf, len);
    int n2 = jam_transform(s, "send", nullptr, copy, buf, len);
    if (n2 < 0) return g_next_send(s, buf, len, flags);
    return g_next_send(s, copy, n2, flags);
}

static int WINAPI hook_wsasend(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD sent,
                               DWORD flags, LPWSAOVERLAPPED ov,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
    if (!g_jamEnabled || !bufs || cnt == 0)
        return g_next_wsasend(s, bufs, cnt, sent, flags, ov, cb);
    SIZE_T total = 0;
    for (DWORD i = 0; i < cnt; ++i) total += bufs[i].len;
    if (total < 8 || total > 65535)
        return g_next_wsasend(s, bufs, cnt, sent, flags, ov, cb);
    char* copy = (char*)_alloca(total);
    SIZE_T o = 0;
    for (DWORD i = 0; i < cnt; ++i) { memcpy(copy + o, bufs[i].buf, bufs[i].len); o += bufs[i].len; }
    int n2 = jam_transform(s, "WSASend", nullptr, copy, copy, (int)total);
    if (n2 < 0) return g_next_wsasend(s, bufs, cnt, sent, flags, ov, cb);
    if ((SIZE_T)n2 > total) n2 = (int)total;
    WSABUF one; one.buf = copy; one.len = (ULONG)n2;
    return g_next_wsasend(s, &one, 1, sent, flags, ov, cb);
}

static int WINAPI hook_wsasendto(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD sent,
                                 DWORD flags, const struct sockaddr* to, int tolen,
                                 LPWSAOVERLAPPED ov,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
    if (!g_jamEnabled || !bufs || cnt == 0)
        return g_next_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cb);
    SIZE_T total = 0;
    for (DWORD i = 0; i < cnt; ++i) total += bufs[i].len;
    if (total < 8 || total > 65535)
        return g_next_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cb);
    char* copy = (char*)_alloca(total);
    SIZE_T o = 0;
    for (DWORD i = 0; i < cnt; ++i) { memcpy(copy + o, bufs[i].buf, bufs[i].len); o += bufs[i].len; }
    int n2 = jam_transform(s, "WSASendTo", to, copy, copy, (int)total);
    if (n2 < 0) return g_next_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cb);
    if ((SIZE_T)n2 > total) n2 = (int)total;
    WSABUF one; one.buf = copy; one.len = (ULONG)n2;
    return g_next_wsasendto(s, &one, 1, sent, flags, to, tolen, ov, cb);
}

// ---------------------------------------------------------------------------
// 热键线程：F8 / Ctrl+F8
// ---------------------------------------------------------------------------
static DWORD WINAPI WorkerProc(LPVOID)
{
    g_hotkeyTid = GetCurrentThreadId();
    if (!RegisterHotKey(nullptr, 1, 0, VK_F8) &&
        !RegisterHotKey(nullptr, 1, MOD_CONTROL, VK_F8))
        jam_log("[hotkey] 注册失败(F8/Ctrl+F8 都被占)\n");
    else
        jam_log("[hotkey] F8 / Ctrl+F8 已注册\n");

    for (;;)
    {
        MSG m;
        if (GetMessageA(&m, nullptr, 0, 0) <= 0) break;
        if (m.message == WM_HOTKEY)
        {
            LONG v = (g_jamEnabled == 0) ? 1 : 0;
            InterlockedExchange(&g_jamEnabled, v);
            shm_publish();
            jam_log("[hotkey] -> %s\n", v ? "干扰开启" : "恢复原始");
        }
        if (g_quit) break;
    }
    UnregisterHotKey(nullptr, 1);
    return 0;
}

// ---------------------------------------------------------------------------
// 控制线程：30ms 轮询共享内存（Mio 写 -> 本进程执行）
// ---------------------------------------------------------------------------
static DWORD WINAPI ControlProc(LPVOID)
{
    for (;;)
    {
        if (g_quit) return 0;
        Sleep(30);
        MioJamShared* c = (MioJamShared*)g_shmView;
        if (!c || c->magic != MIO_JAM_MAGIC) continue;

        if (c->reqUnload != 0)   // Mio 请求卸载
        {
            g_quit = 1;
            UnregisterHotKey(nullptr, 1);
            if (g_hotkeyTid) PostThreadMessageA(g_hotkeyTid, WM_QUIT, 0, 0);
            if (g_orig_sendto)    { MH_DisableHook((void*)g_orig_sendto);    MH_RemoveHook((void*)g_orig_sendto); }
            if (g_orig_send)      { MH_DisableHook((void*)g_orig_send);      MH_RemoveHook((void*)g_orig_send); }
            if (g_orig_wsasend)   { MH_DisableHook((void*)g_orig_wsasend);   MH_RemoveHook((void*)g_orig_wsasend); }
            if (g_orig_wsasendto) { MH_DisableHook((void*)g_orig_wsasendto); MH_RemoveHook((void*)g_orig_wsasendto); }
            MH_Uninitialize();
            if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
            if (g_shmView) UnmapViewOfFile(g_shmView);
            if (g_shmMap)  CloseHandle(g_shmMap);
            FreeLibraryAndExitThread(g_hinst, 0);
        }
        if (c->jamSwitch != (uint32_t)g_jamEnabled) InterlockedExchange(&g_jamEnabled, (LONG)c->jamSwitch);
        if (c->payloadType != (uint32_t)g_payloadType) g_payloadType = (LONG)c->payloadType;
        if (c->mode != (uint32_t)g_mode) g_mode = (LONG)c->mode;
        shm_publish();
    }
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_hinst = inst;
    DisableThreadLibraryCalls(inst);
    srand(GetTickCount());

    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    if (!ws) ws = LoadLibraryA("ws2_32.dll");
    int ok_to = 0, ok_sd = 0, ok_ws = 0, ok_wt = 0;
    if (ws)
    {
        g_orig_sendto    = GetProcAddress(ws, "sendto");
        g_orig_send      = GetProcAddress(ws, "send");
        g_orig_wsasend   = GetProcAddress(ws, "WSASend");
        g_orig_wsasendto = GetProcAddress(ws, "WSASendTo");
        MH_Initialize();
        if (g_orig_sendto    && MH_CreateHook((void*)g_orig_sendto,    (void*)&hook_sendto,    (void**)&g_next_sendto)    == MH_OK && MH_EnableHook((void*)g_orig_sendto)    == MH_OK) ok_to = 1;
        if (g_orig_send      && MH_CreateHook((void*)g_orig_send,      (void*)&hook_send,      (void**)&g_next_send)      == MH_OK && MH_EnableHook((void*)g_orig_send)      == MH_OK) ok_sd = 1;
        if (g_orig_wsasend   && MH_CreateHook((void*)g_orig_wsasend,   (void*)&hook_wsasend,   (void**)&g_next_wsasend)   == MH_OK && MH_EnableHook((void*)g_orig_wsasend)   == MH_OK) ok_ws = 1;
        if (g_orig_wsasendto && MH_CreateHook((void*)g_orig_wsasendto, (void*)&hook_wsasendto, (void**)&g_next_wsasendto) == MH_OK && MH_EnableHook((void*)g_orig_wsasendto) == MH_OK) ok_wt = 1;
    }

    g_shmMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(MioJamShared), MIO_JAM_SHARED_NAME);
    if (g_shmMap)
    {
        g_shmView = MapViewOfFile(g_shmMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioJamShared));
        if (g_shmView)
        {
            MioJamShared* c = (MioJamShared*)g_shmView;
            // 关键：只有"首发进程"初始化，后续注入的进程不许清空（否则会把 Mio 面板的
            //       开关/模式一起抹掉）。原外挂用一个莫名其妙的 `pid != 183` 干这事。
            if (c->magic != MIO_JAM_MAGIC)
            {
                memset(c, 0, sizeof(*c));
                c->magic       = MIO_JAM_MAGIC;
                c->jamSwitch   = 0;   // 默认关：由 Mio 面板决定
                c->initialized = 1;
                c->mode        = (uint32_t)g_mode;
                c->payloadType = (uint32_t)g_payloadType;
            }
            c->pid = GetCurrentProcessId();
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, path, MAX_PATH))
            {
                wchar_t* p = wcsrchr(path, L'\\');
                wcsncpy(c->exeName, p ? p + 1 : path, 0x3F);
                c->exeName[0x3E] = 0;
            }
        }
        else { CloseHandle(g_shmMap); g_shmMap = nullptr; }
    }

    stat_ensure();
    HANDLE t1 = CreateThread(nullptr, 0, WorkerProc,  nullptr, 0, nullptr);
    HANDLE t2 = CreateThread(nullptr, 0, ControlProc, nullptr, 0, nullptr);
    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);

    jam_log("---- attach [%s] PID=%lu hook sendto=%d send=%d WSASend=%d WSASendTo=%d ----\n",
            MioJammerBuildTag, (unsigned long)GetCurrentProcessId(), ok_to, ok_sd, ok_ws, ok_wt);
    return TRUE;
}

// ---------------------------------------------------------------------------
// 导出（Mio 也可直接远程调用 / 或用 GetProcAddress 拿这些）
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void MioJamOn(void)     { InterlockedExchange(&g_jamEnabled, 1); shm_publish(); }
extern "C" __declspec(dllexport) void MioJamOff(void)    { InterlockedExchange(&g_jamEnabled, 0); shm_publish(); }
extern "C" __declspec(dllexport) int  MioJamState(void)  { return g_jamEnabled; }
extern "C" __declspec(dllexport) void MioJamSetMode(int m)  { g_mode = m; shm_publish(); }
extern "C" __declspec(dllexport) void MioJamSetPayloadType(int pt) { g_payloadType = pt; shm_publish(); }
