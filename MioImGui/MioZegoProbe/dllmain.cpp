// ---------------------------------------------------------------------------
// MioZegoProbe.dll —— 注入 TT语音娱乐版 renderer 进程的码率探针载荷
//
// 流程：
//   1. DllMain 起一个工作线程；创建/打开命名共享内存 Local\MioZegoKbps_v1。
//   2. 等待 ZegoExpressEngine.dll 加载完成。
//   3. MinHook inline hook 两个导出（前 5 字节 E9 跳转，等价 Nova 报告手法）：
//        zego_register_player_quality_update_callback
//        zego_register_publisher_quality_update_callback
//        zego_register_room_stream_update_callback（成员昵称 streamID->userName 映射）
//   4. 当宿主（ZegoExpressNodeNative.node）注册回调时，把它的回调指针换成
//      我们的包装函数，user_data 原样保留；包装函数读取质量结构体里的
//      audioKBPS / videoKBPS / rtt 写入共享内存后，再把参数原样转发给宿主
//      原回调 —— 宿主自身行为完全不变。
//   5. 首次收到回调时落下校准转储（%LOCALAPPDATA%\Mio\zego-probe\），
//      便于偏移对照排查。
//
// 结构体偏移（ZegoExpressNodeNative.node 3.21.0 反汇实测）：
//   play    .audioKBPS @ +0x50, .videoKBPS @ +0x20, .rtt @ +0x68 (int32)
//   publish .audioKBPS @ +0x30, .videoKBPS @ +0x18, .rtt @ +0x38 (int32)
//
// 本模块一经注入不卸载（钩子跳板生命周期跟随模块），进程退出即自动清理。
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cmath>
#include <string>

#include "MinHook.h"
#include "MioZegoProbeShared.h"

// 构建印记（导出后可直接在二进制里检索，用于确认版本）
extern "C" __declspec(dllexport) const char MioProbeBuildTag[] =
    "MioZegoProbe v25 (boost-default-400 + dsp-clean + agc-smooth)";

// ---------------------------------------------------------------------------
// 共享内存

static HANDLE          g_mapping = nullptr;
static MioZegoShared*  g_shared = nullptr;
static CRITICAL_SECTION g_lock;
static bool            g_lock_ready = false;
static volatile LONG   g_win_pub_kbps = 0; // 侧信息补差基准：窗口内本机推流底流最大 kbps（boost 线程每秒取走）

// ---- 音频电平共享内存（监听页电平条数据源）----
static HANDLE                 g_level_mapping = nullptr;
static MioAudioLevelShared*   g_level = nullptr;
static volatile LONG         g_injector_alive = 0; // 注入器心跳状态（由工作循环维护）

static bool EnsureLevelShared()
{
    if (g_level)
        return true;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(MioAudioLevelShared), MIO_LEVEL_SHARED_NAME);
    if (!mapping)
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_LEVEL_SHARED_NAME);
    if (!mapping)
        return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioAudioLevelShared));
    if (!view)
    {
        CloseHandle(mapping);
        return false;
    }
    g_level_mapping = mapping;
    g_level = (MioAudioLevelShared*)view;
    memset(g_level, 0, sizeof(*g_level));
    g_level->magic = MIO_LEVEL_SHARED_MAGIC;
    g_level->version = 1;
    return true;
}

static bool EnsureShared()
{
    if (g_shared)
        return true;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(MioZegoShared), MIO_ZEGO_SHARED_NAME);
    if (!mapping)
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_ZEGO_SHARED_NAME);
    if (!mapping)
        return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioZegoShared));
    if (!view)
    {
        CloseHandle(mapping);
        return false;
    }

    g_mapping = mapping;
    g_shared = (MioZegoShared*)view;

    // 每次载荷启动全量重置：旧进程遗留的条目不应继续显示。
    std::memset(g_shared, 0, sizeof(MioZegoShared));
    g_shared->magic = MIO_ZEGO_SHARED_MAGIC;
    g_shared->version = MIO_ZEGO_SHARED_VERSION;
    g_shared->pid = GetCurrentProcessId();
    g_shared->payload_boot_ms = GetTickCount64();
    g_shared->flags |= MIO_ZEGO_FLAG_DLL_READY;
    return true;
}

// ---------------------------------------------------------------------------
// 可读性防护 / 校准转储

static bool IsReadable(const void* p, size_t bytes)
{
    if (!p || bytes == 0 || bytes > (1u << 20))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const uintptr_t start = (uintptr_t)p;
    const uintptr_t end = start + bytes;
    const uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end <= region_end;
}

static void WriteDump(uint32_t dir, const char* stream_id, const void* quality)
{
    static bool dumped_play = false;
    static bool dumped_pub = false;
    bool& done = (dir == MIO_ZEGO_DIR_PLAY) ? dumped_play : dumped_pub;
    if (done)
        return;
    done = true;

    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;

    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\probe-dump-%lu.bin",
                 base, (unsigned long)GetCurrentProcessId());

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;

    char head[320] = {};
    const int n = snprintf(head, sizeof(head),
                           "dir=%s tick=%llu stream=%s struct=%p\n",
                           dir == MIO_ZEGO_DIR_PLAY ? "play" : "publish",
                           (unsigned long long)GetTickCount64(),
                           stream_id ? stream_id : "(null)", quality);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);
    if (IsReadable(quality, 0x100))
        fwrite(quality, 1, 0x100, f);
    fclose(f);
}

// 流列表回调原始数据转储（前 2 次回调）：用于核对外部结构体字段布局
static void DumpStreamBlocks(const char* room_id, int update_type, const void* blocks,
                             int count, const void* a5)
{
    static int dumped = 0;
    if (dumped >= 2)
        return;
    ++dumped;

    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\stream-dump-%lu-%d.bin",
                 base, (unsigned long)GetCurrentProcessId(), dumped);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    char head[320] = {};
    const int n = snprintf(head, sizeof(head), "room=%s type=%d count=%d blocks=%p a5=%p\n",
                           room_id ? room_id : "(null)", update_type, count, blocks, a5);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);
    if (blocks && count > 0)
    {
        size_t total = (size_t)count * 0x640;
        if (total > 12u * 0x640u)
            total = 12u * 0x640u;
        if (IsReadable(blocks, total))
            fwrite(blocks, 1, total, f);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// 条目写入

// 昵称待定表：流列表回调可能先于质量回调到达，先把映射记下来，
// 等对应条目创建时回填。
#define MIO_PENDING_NAMES 128
static char g_pending_sid[MIO_PENDING_NAMES][MIO_ZEGO_STREAM_ID_MAX];
static char g_pending_name[MIO_PENDING_NAMES][MIO_ZEGO_NAME_MAX];

static void CopyString(char* dst, size_t cap, const char* src)
{
    size_t n = 0;
    while (n + 1 < cap && src[n])
        ++n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// 定长缓冲区读取：最多 field_max 字节，自行找 NUL，防止越界
static void BoundedStr(char* dst, size_t cap, const char* src, size_t field_max)
{
    size_t n = 0;
    while (n < field_max && n + 1 < cap && src[n])
        ++n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// 昵称落库：命中现有条目则直接更新，否则存待定表
static void ApplyName(const char* sid, const char* name)
{
    if (!g_shared || !g_lock_ready || !sid || !sid[0] || !name || !name[0])
        return;
    EnterCriticalSection(&g_lock);
    for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
    {
        MioZegoStreamEntry& e = g_shared->entries[i];
        if (e.stream_id[0] && strncmp(e.stream_id, sid, MIO_ZEGO_STREAM_ID_MAX - 1) == 0)
            CopyString(e.user_name, MIO_ZEGO_NAME_MAX, name);
    }
    int empty = -1;
    for (int i = 0; i < MIO_PENDING_NAMES; ++i)
    {
        if (g_pending_sid[i][0] && strncmp(g_pending_sid[i], sid, MIO_ZEGO_STREAM_ID_MAX - 1) == 0)
        {
            CopyString(g_pending_name[i], MIO_ZEGO_NAME_MAX, name);
            empty = -2;
            break;
        }
        if (empty < 0 && !g_pending_sid[i][0])
            empty = i;
    }
    if (empty >= 0)
    {
        CopyString(g_pending_sid[empty], MIO_ZEGO_STREAM_ID_MAX, sid);
        CopyString(g_pending_name[empty], MIO_ZEGO_NAME_MAX, name);
    }
    LeaveCriticalSection(&g_lock);
}

// 条目摘除（流停止/成员离开）：清空该 stream 的所有方向条目
static void RemoveEntry(const char* sid)
{
    if (!g_shared || !g_lock_ready || !sid || !sid[0])
        return;
    EnterCriticalSection(&g_lock);
    g_shared->seq++;
    uint32_t count = 0;
    for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
    {
        MioZegoStreamEntry& e = g_shared->entries[i];
        if (e.stream_id[0] && strncmp(e.stream_id, sid, MIO_ZEGO_STREAM_ID_MAX - 1) == 0)
            std::memset(&e, 0, sizeof(e));
        if (g_shared->entries[i].stream_id[0])
            ++count;
    }
    g_shared->entry_count = count;
    g_shared->seq++;
    LeaveCriticalSection(&g_lock);
}

// 解析一批流块（ADD 记名字 / DELETE 摘条目），注册路径与包装函数路径共用
static void ParseStreamBlockList(int update_type, const void* blocks, int count)
{
    if (!blocks || count <= 0 || count > 256)
        return;
    const unsigned char* base = (const unsigned char*)blocks;
    for (int i = 0; i < count; ++i)
    {
        const unsigned char* e = base + (size_t)i * 0x640;
        if (!IsReadable(e, 0x340))
            break;
        // 实测字段布局：+0x00 用户ID / +0x40 昵称(UTF-8) / +0x140 流ID / +0x240 扩展JSON
        char sid_buf[MIO_ZEGO_STREAM_ID_MAX] = {};
        BoundedStr(sid_buf, sizeof(sid_buf), (const char*)(e + 0x140), 0x100);
        if (!sid_buf[0])
            continue;
        if (update_type == 0)
        {
            char name_buf[MIO_ZEGO_NAME_MAX] = {};
            BoundedStr(name_buf, sizeof(name_buf), (const char*)(e + 0x40), 0x100);
            if (name_buf[0])
                ApplyName(sid_buf, name_buf);
        }
        else
        {
            RemoveEntry(sid_buf);
        }
    }
}

// ---------------------------------------------------------------------------
// 组件流块转换器钩子（ZegoExpressNodeNative.node + 0x3CC0）
// 每次「房间流更新」都会经过它，与回调注册时机无关 —— 负责：
//   1) 原始流块转储（前 5 次，用于核对字段布局）
//   2) 兜底抓取昵称并落库（即使注册未捕获也能拿到名字）

typedef void* (*ZegoStreamConvFn)(void* dest, const void* block);
static ZegoStreamConvFn g_orig_conv = nullptr;
static int g_conv_dump_count = 0;

static void DumpConvertBlock(const void* block)
{
    if (g_conv_dump_count >= 5)
        return;
    if (!block || !IsReadable(block, 0x340))
        return;
    ++g_conv_dump_count;

    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\conv-dump-%lu-%d.bin",
                 base, (unsigned long)GetCurrentProcessId(), g_conv_dump_count);

    const unsigned char* e = (const unsigned char*)block;
    char f0[128] = {};
    char f1[128] = {};
    char f2[128] = {};
    char f3[128] = {};
    BoundedStr(f0, sizeof(f0), (const char*)e, 0x40);
    BoundedStr(f1, sizeof(f1), (const char*)(e + 0x40), 0x100);
    BoundedStr(f2, sizeof(f2), (const char*)(e + 0x140), 0x100);
    BoundedStr(f3, sizeof(f3), (const char*)(e + 0x240), 0x100);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    char head[768] = {};
    const int n = snprintf(head, sizeof(head),
                           "conv block=%p\n  uid(+0x00)=[%s]\n  name(+0x40)=[%s]\n"
                           "  sid(+0x140)=[%s]\n  ext(+0x240)=[%s]\n",
                           block, f0, f1, f2, f3);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);
    fwrite(block, 1, 0x640, f);
    fclose(f);
}

static void* OnStreamConvert(void* dest, const void* block)
{
    if (block && IsReadable(block, 0x340))
    {
        DumpConvertBlock(block);

        const unsigned char* e = (const unsigned char*)block;
        char sid[96] = {};
        char nm[64] = {};
        BoundedStr(sid, sizeof(sid), (const char*)(e + 0x140), 0x100);
        BoundedStr(nm, sizeof(nm), (const char*)(e + 0x40), 0x100);
        if (sid[0] && nm[0])
            ApplyName(sid, nm);
    }
    if (g_orig_conv)
        return g_orig_conv(dest, block);
    return dest;
}

static void UpdateEntry(uint32_t dir, const char* stream_id, const void* quality)
{
    if (!g_shared || !stream_id || !quality)
        return;

    size_t len = 0;
    while (len < MIO_ZEGO_STREAM_ID_MAX - 1 && stream_id[len])
        ++len;
    if (len == 0)
        return;

    const bool play = (dir == MIO_ZEGO_DIR_PLAY);
    const size_t off_audio = play ? 0x50 : 0x30;
    const size_t off_video = play ? 0x20 : 0x18;
    const size_t off_rtt = play ? 0x68 : 0x38;
    if (!IsReadable(quality, off_rtt + 4))
    {
        InterlockedIncrement((volatile LONG*)&g_shared->bad_struct_count);
        return;
    }

    double audio = 0.0;
    double video = 0.0;
    int32_t rtt = 0;
    memcpy(&audio, (const uint8_t*)quality + off_audio, sizeof(audio));
    memcpy(&video, (const uint8_t*)quality + off_video, sizeof(video));
    memcpy(&rtt, (const uint8_t*)quality + off_rtt, sizeof(rtt));

    if (!(audio >= 0.0 && audio < 100000.0))
    {
        InterlockedIncrement((volatile LONG*)&g_shared->bad_struct_count);
        return;
    }
    if (!(video >= 0.0 && video < 100000.0))
        video = 0.0;
    if (rtt < 0 || rtt > 100000)
        rtt = 0;

    EnterCriticalSection(&g_lock);
    g_shared->seq++;

    MioZegoStreamEntry* slot = nullptr;
    MioZegoStreamEntry* empty = nullptr;
    MioZegoStreamEntry* oldest = nullptr;
    for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
    {
        MioZegoStreamEntry& e = g_shared->entries[i];
        if (e.direction == dir && e.stream_id[0] &&
            strncmp(e.stream_id, stream_id, MIO_ZEGO_STREAM_ID_MAX - 1) == 0)
        {
            slot = &e;
            break;
        }
        if (!empty && e.stream_id[0] == 0 && e.seen_count == 0)
            empty = &e;
        if (!oldest || e.last_seen_ms < oldest->last_seen_ms)
            oldest = &e;
    }
    if (!slot)
    {
        slot = empty ? empty : oldest;
        std::memset(slot, 0, sizeof(*slot));
    }

    memcpy(slot->stream_id, stream_id, len);
    slot->stream_id[len] = 0;

    // 昵称回填（待定表中可能有先到的映射）
    for (int i = 0; i < MIO_PENDING_NAMES; ++i)
    {
        if (g_pending_sid[i][0] &&
            strncmp(g_pending_sid[i], stream_id, MIO_ZEGO_STREAM_ID_MAX - 1) == 0)
        {
            CopyString(slot->user_name, MIO_ZEGO_NAME_MAX, g_pending_name[i]);
            break;
        }
    }

    slot->audio_kbps = audio;
    if (!play)
    {
        // 侧信息补差基准：滚动记录本机推流底流最大值（boost 线程每秒取走归零）
        const LONG v = (LONG)(audio + 0.5);
        LONG cur = g_win_pub_kbps;
        while (v > cur)
        {
            const LONG prev = InterlockedCompareExchange((volatile LONG*)&g_win_pub_kbps, v, cur);
            if (prev == cur)
                break;
            cur = prev;
        }
    }
    slot->video_kbps = video;
    slot->rtt_ms = rtt;
    slot->direction = dir;
    slot->seen_count++;
    slot->last_seen_ms = GetTickCount64();
    if (play)
        g_shared->play_cb_count++;
    else
        g_shared->pub_cb_count++;

    uint32_t count = 0;
    for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
        if (g_shared->entries[i].stream_id[0])
            ++count;
    g_shared->entry_count = count;

    g_shared->seq++;
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// 回调与钩子
//
// 实测 ABI（.node 3.21.0）：
//   注册函数： (回调指针, user_data)           —— 2 参
//   质量回调： (stream_id字符串, 结构体指针, user_data, ...) —— rcx=串, rdx=结构体
// 为兼容起见统一按 4 参读写，多出的寄存器参数原样透传（x64 调用约定下安全）。

typedef void (*ZegoRegisterFn)(void* cb, void* user_data, void* a3, void* a4);
typedef void (*ZegoQualityCb)(const char* stream_id, void* quality, void* user_data, void* a4);

static ZegoRegisterFn g_orig_register_play = nullptr;
static ZegoRegisterFn g_orig_register_pub = nullptr;
static void* g_app_play_cb = nullptr;
static void* g_app_pub_cb = nullptr;

static void CaptureQuality(uint32_t dir, const char* stream_id, const void* quality)
{
    if (!g_shared)
        return;
    InterlockedOr((volatile LONG*)&g_shared->flags,
                  dir == MIO_ZEGO_DIR_PLAY ? MIO_ZEGO_FLAG_PLAY_INVOKED : MIO_ZEGO_FLAG_PUB_INVOKED);
    WriteDump(dir, stream_id, quality);
    UpdateEntry(dir, stream_id, quality);
}

static void OnPlayQuality(const char* stream_id, void* quality, void* user_data, void* a4)
{
    CaptureQuality(MIO_ZEGO_DIR_PLAY, stream_id, quality);
    if (g_app_play_cb)
        ((ZegoQualityCb)g_app_play_cb)(stream_id, quality, user_data, a4);
}

static void OnPubQuality(const char* stream_id, void* quality, void* user_data, void* a4)
{
    CaptureQuality(MIO_ZEGO_DIR_PUBLISH, stream_id, quality);
    if (g_app_pub_cb)
        ((ZegoQualityCb)g_app_pub_cb)(stream_id, quality, user_data, a4);
}

static void OnRegisterPlay(void* cb, void* user_data, void* a3, void* a4)
{
    if (g_shared)
        InterlockedIncrement((volatile LONG*)&g_shared->register_player_calls);
    g_app_play_cb = cb;
    if (g_orig_register_play)
        g_orig_register_play(cb ? (void*)&OnPlayQuality : nullptr, user_data, a3, a4);
}

static void OnRegisterPub(void* cb, void* user_data, void* a3, void* a4)
{
    if (g_shared)
        InterlockedIncrement((volatile LONG*)&g_shared->register_pub_calls);
    g_app_pub_cb = cb;
    if (g_orig_register_pub)
        g_orig_register_pub(cb ? (void*)&OnPubQuality : nullptr, user_data, a3, a4);
}

// 流列表回调（昵称来源）：
//   rcx=room_id 字符串, rdx=update_type, r8=流块数组指针, r9=块数量, 第五参=附加字符串。
//   每块 0x640 字节：stream_id@+0x00(64) / user_id@+0x40(256)
//   / user_name@+0x140(256) / extended@+0x240(1024)。
typedef void (*ZegoStreamUpdateCb)(const char* room_id, int update_type,
                                   const void* stream_blocks, int stream_count,
                                   const void* a5, const void* a6);
static ZegoRegisterFn g_orig_register_stream = nullptr;
static void* g_app_stream_cb = nullptr;

static void OnStreamUpdate(const char* room_id, int update_type, const void* stream_blocks,
                           int stream_count, const void* a5, const void* a6)
{
    if (g_shared)
        InterlockedOr((volatile LONG*)&g_shared->flags, MIO_ZEGO_FLAG_STREAM_INVOKED);
    DumpStreamBlocks(room_id, update_type, stream_blocks, stream_count, a5);
    ParseStreamBlockList(update_type, stream_blocks, stream_count);
    if (g_app_stream_cb)
        ((ZegoStreamUpdateCb)g_app_stream_cb)(room_id, update_type, stream_blocks, stream_count, a5, a6);
}

static void OnRegisterStream(void* cb, void* user_data, void* a3, void* a4)
{
    if (g_shared)
        InterlockedIncrement((volatile LONG*)&g_shared->register_stream_calls);
    g_app_stream_cb = cb;
    if (g_orig_register_stream)
        g_orig_register_stream(cb ? (void*)&OnStreamUpdate : nullptr, user_data, a3, a4);
}

// ---------------------------------------------------------------------------
// 兜底直挂：三个事件包装函数本体（ZegoExpressNodeNative.node 内，RVA 见下）
// 引擎每次都直接调用这些包装函数 —— 与「回调注册」时机完全无关，
// 即使注册在我们注入之前就已完成，数据仍然会被我们截获（彻底消除竞态）。

typedef void (*QualityWrapperFn)(const char* stream_id, void* quality, void* a3, void* a4);
static QualityWrapperFn g_orig_w_play = nullptr;
static QualityWrapperFn g_orig_w_pub = nullptr;

static void OnPlayQualityWrapper(const char* stream_id, void* quality, void* a3, void* a4)
{
    CaptureQuality(MIO_ZEGO_DIR_PLAY, stream_id, quality);
    if (g_orig_w_play)
        g_orig_w_play(stream_id, quality, a3, a4);
}

static void OnPubQualityWrapper(const char* stream_id, void* quality, void* a3, void* a4)
{
    CaptureQuality(MIO_ZEGO_DIR_PUBLISH, stream_id, quality);
    if (g_orig_w_pub)
        g_orig_w_pub(stream_id, quality, a3, a4);
}

typedef void (*StreamWrapperFn)(const char* room_id, int update_type, const void* blocks,
                                int count, const void* a5, const void* a6);
static StreamWrapperFn g_orig_w_stream = nullptr;

static void OnStreamUpdateWrapper(const char* room_id, int update_type, const void* blocks,
                                  int count, const void* a5, const void* a6)
{
    if (g_shared)
        InterlockedOr((volatile LONG*)&g_shared->flags, MIO_ZEGO_FLAG_STREAM_INVOKED);
    DumpStreamBlocks(room_id, update_type, blocks, count, a5);
    ParseStreamBlockList(update_type, blocks, count);
    if (g_orig_w_stream)
        g_orig_w_stream(room_id, update_type, blocks, count, a5, a6);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 4000kbps 侧信息加持（原生版，思路源自 patches/sideInfoBoostNative）：
//   每 ~20ms 调 zego_express_send_audio_side_info 发送 ≤18 个 ≤1000B 零填充包，
//   把真实传输字节顶上去；目标 kbps 可用 %LOCALAPPDATA%\Mio\boost-target.txt 调整。
typedef int (*SendAudioSideInfoFn)(void* data, uint32_t data_length, uint32_t pad, uint32_t channel,
                                   double f0, double f1, double ts);
static SendAudioSideInfoFn g_fn_side_info = nullptr;
static volatile LONG g_boost_target = 400;
static unsigned char g_boost_bufs[18][1000] = {};
static int g_boost_lens[18] = {};
static volatile LONG g_boost_count = 0;
static volatile LONG g_boost_bytes = 0;
static volatile LONG g_boost_bad = 0;
static DWORD64 g_boost_last_capture = 0;
typedef int (*SetEngineConfigFn)(void* config);
static SetEngineConfigFn g_fn_set_engine_config = nullptr;
static void BoostRebuildChunks(int target, int base)
{
    if (target <= 0)
    {
        g_boost_count = 0;
        g_boost_bytes = 0;
        return; // 0 = 关闭侧信息（纯底流）
    }
    if (target > 20000)
        target = 20000;
    if (base < 0)
        base = 0;
    if (base > 2000)
        base = 2000;
    // 动态补差：按【目标 − 当前底流】算侧信息量，合计 ≈ 目标（底流多少都对齐）
    long long need = ((long long)(target - base) * 1000) / 404;
    if (need <= 0)
    {
        g_boost_count = 0;
        g_boost_bytes = 0;
        return; // 底流已 ≥ 目标：不加侧信息
    }
    if (need > 18000)
        need = 18000; // 包组物理上限（18 × 1000B）
    int pk = (int)((need + 999) / 1000);
    if (pk > 18)
        pk = 18;
    if (pk < 1)
        pk = 1;
    int per = (int)(need / pk);
    if (per < 1)
        per = 1;
    if (per > 1000)
        per = 1000;
    int last = (int)(need - (long long)per * (pk - 1));
    if (last < 1)
        last = 1;
    if (last > 1000)
        last = 1000;
    for (int i = 0; i < pk - 1; ++i)
        g_boost_lens[i] = per;
    g_boost_lens[pk - 1] = last;
    g_boost_count = pk;
    g_boost_bytes = per * (pk - 1) + last;
}
// 侧信息速率回写共享内存 reserved0（低32=每帧字节，高32=折算kbps）。
// 推流端统计不含侧信息、拉流端统计含；Mio 房间页据此显示「底流+侧信息」真实合计。
static void BoostPublishToShared()
{
    if (!g_shared)
        return;
    const uint32_t bf = (uint32_t)g_boost_bytes;
    const uint32_t kb = (uint32_t)(((unsigned long long)bf * 400ull) / 1000ull); // 50fps × 8bit ÷ 1000
    g_shared->reserved0 = ((uint64_t)kb << 32) | (uint64_t)bf;
}
static void BoostLogFirst(const char* line)
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\boost-native-%lu.log",
                 base, (unsigned long)GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
}
static int ReadBoostTargetFile()
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return 0;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\boost-target.txt", base);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt") != 0 || !f)
        return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1)
        v = 0;
    fclose(f);
    return v;
}
static void TryDisableDtx() // v21 起禁用：v20 实测该调用引发引擎崩溃
{
    return; // 紧急回滚：不再调用 set_engine_config
    if (!g_fn_set_engine_config)
        return;
    static DWORD64 last = 0;
    const DWORD64 nowd = GetTickCount64();
    if (last != 0 && nowd - last < 10000)
        return;
    last = nowd;
    struct ZegoEngineConfigLite
    {
        const char* log_config;
        const char* advanced_config;
        const char* extra0;
        const char* extra1;
    };
    ZegoEngineConfigLite cfg = {};
    cfg.advanced_config = "{\"enable_dtx\":\"false\"}";
    g_fn_set_engine_config(&cfg);
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0)
        BoostLogFirst("dtx-off called (advanced_config enable_dtx=false)");
}
static double BoostUnixMs()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = {};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}
static void BoostSendBurstOnce()
{
    if (!g_fn_side_info || g_boost_count <= 0)
        return;
    const double ts = BoostUnixMs();
    const int count = (int)g_boost_count;
    int bad = 0;
    for (int i = 0; i < count; ++i)
    {
        const int rc = g_fn_side_info(g_boost_bufs[i], (uint32_t)g_boost_lens[i], 0, 0, 0.0, 0.0, ts);
        if (rc != 0)
            bad++;
    }
    static volatile LONG frames = 0;
    const LONG bf = InterlockedIncrement(&frames);
    if (bad)
        InterlockedExchangeAdd(&g_boost_bad, bad);
    if (bf <= 10 || (bf % 250) == 0)
    {
        char line[176] = {};
        snprintf(line, sizeof(line), "frame#%ld rc_bad=%d total_bad=%ld chunks=%d bytes=%ld target=%ld",
                 (long)bf, bad, (long)g_boost_bad, count, (long)g_boost_bytes, (long)g_boost_target);
        BoostLogFirst(line);
    }
}
static DWORD WINAPI BoostThreadProc(LPVOID)
{
    for (;;)
    {
        ::Sleep(20);
        const DWORD64 nowb = GetTickCount64();
        if (g_boost_last_capture != 0 && nowb - g_boost_last_capture <= 3000)
            BoostSendBurstOnce();
    }
    return 0;
}
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// DSP 净化（v24）：强制关闭即构自带 AGC/ANS/瞬态降噪/人声增强 —— 防止 SDK 内置
// 处理器与我们的增益互相打架（声音一顿一顿的另一源头，参考 Nova 取证报告 hook 清单）
typedef void (*EnableBoolFn2)(bool enable);
static EnableBoolFn2 g_orig_enable_agc = nullptr;
static EnableBoolFn2 g_orig_enable_ans = nullptr;
static EnableBoolFn2 g_orig_enable_tans = nullptr;
static EnableBoolFn2 g_orig_enable_sphenh = nullptr;
static void OnEnableAgc(bool) { if (g_orig_enable_agc) g_orig_enable_agc(false); }
static void OnEnableAns(bool) { if (g_orig_enable_ans) g_orig_enable_ans(false); }
static void OnEnableTans(bool) { if (g_orig_enable_tans) g_orig_enable_tans(false); }
static void OnEnableSphenh(bool) { if (g_orig_enable_sphenh) g_orig_enable_sphenh(false); }
static void ForceCleanDsp()
{
    static DWORD64 last = 0;
    const DWORD64 nowd = GetTickCount64();
    if (last != 0 && nowd - last < 10000)
        return;
    last = nowd;
    if (g_orig_enable_agc) g_orig_enable_agc(false);
    if (g_orig_enable_ans) g_orig_enable_ans(false);
    if (g_orig_enable_tans) g_orig_enable_tans(false);
    if (g_orig_enable_sphenh) g_orig_enable_sphenh(false);
}
// ---------------------------------------------------------------------------
// 采集处理回调（增益注入点）（ZegoExpressNodeNative.node + 0x26B40）
// 实参形态：rcx=a1, edx=a2(32位), r8=a3(指向2×int的输入输出结构), xmm3=d3。
// 先以探针模式转储观测（不改数据），确认「数据指针 + 长度」后再启用增益。

typedef void (*CaptureWrapperFn)(void* a1, void* a2, void* a3,
                                 double d0, double d1, double d2, double d3);
static CaptureWrapperFn g_orig_w_capture = nullptr;
static ZegoRegisterFn g_orig_register_capture = nullptr;
static void* g_app_capture_cb = nullptr;

static unsigned long long BitsOfDouble(double v)
{
    unsigned long long u = 0;
    memcpy(&u, &v, sizeof(u));
    return u;
}

// ---- 增益：自适应稳定档（上限 16×，目标峰值 ~14000，慢平滑，防时大时小）----
#define MIO_GAIN_MILLI 16000L
static float g_agc_gain = 16.0f; // 当前自适应增益
static float g_agc_env = 0.0f;   // 输入峰值包络（快攻慢放）

// 就地自适应增益（AGC）：包络检测 + 慢跟随增益 + 软膝限幅；长度非法/不可读则跳过
static void ApplyGainToCapture(void* data, int len)
{
    if (!data || len < 2 || len > (1 << 20) || (len & 1))
        return;
    if (!IsReadable(data, (size_t)len))
        return;
    short* s = (short*)data;
    const int count = len / 2;
    int peak = 0;
    for (int i = 0; i < count; ++i)
    {
        const int av = s[i] < 0 ? -s[i] : s[i];
        if (av > peak)
            peak = av;
    }
    const float p = (float)peak;
    if (p > g_agc_env)
        g_agc_env += (p - g_agc_env) * 0.50f;  // 快攻
    else
        g_agc_env += (p - g_agc_env) * 0.0015f; // 慢放（约 2.5s：词间停顿不塌陷，杜绝一顿一顿）
    float want = 14000.0f / (g_agc_env < 400.0f ? 400.0f : g_agc_env);
    if (want > 16.0f)
        want = 16.0f;
    if (want < 1.0f)
        want = 1.0f;
    g_agc_gain += (want - g_agc_gain) * 0.002f; // 增益更慢跟随（约 2.5s），稳如老狗
    for (int i = 0; i < count; ++i)
    {
        float v = (float)s[i] * g_agc_gain;
        const float av = v < 0 ? -v : v;
        if (av > 26000.0f)
            v = (v < 0 ? -1.0f : 1.0f) * (26000.0f + (av - 26000.0f) * 0.333f); // 软膝
        if (v > 32000.0f)
            v = 32000.0f;
        else if (v < -32000.0f)
            v = -32000.0f;
        s[i] = (short)v;
    }
}

static void DumpCaptureArgs(void* a1, void* a2, void* a3,
                            double d0, double d1, double d2, double d3,
                            const short* before16, bool have_before)
{
    static volatile LONG dumped = 0;
    if (InterlockedIncrement(&dumped) > 5)
        return;
    const long dn = dumped;

    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\capture-dump-%lu-%ld.bin",
                 base, (unsigned long)GetCurrentProcessId(), dn);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;

    char head[1024] = {};
    int n = snprintf(head, sizeof(head),
                     "capture a1=%p a2=%p a3=%p\n"
                     "  d0bits=%016llx d1bits=%016llx d2bits=%016llx d3bits=%016llx\n"
                     "  d0d=%.6f d1d=%.6f d2d=%.6f d3d=%.6f\n",
                     a1, a2, a3,
                     BitsOfDouble(d0), BitsOfDouble(d1), BitsOfDouble(d2), BitsOfDouble(d3),
                     d0, d1, d2, d3);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);

    n = snprintf(head, sizeof(head), "  gain_milli=%ld\n", (long)MIO_GAIN_MILLI);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);
    if (have_before)
    {
        n = snprintf(head, sizeof(head),
                     "  before16: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                     before16[0], before16[1], before16[2], before16[3],
                     before16[4], before16[5], before16[6], before16[7],
                     before16[8], before16[9], before16[10], before16[11],
                     before16[12], before16[13], before16[14], before16[15]);
        if (n > 0)
            fwrite(head, 1, (size_t)n, f);
    }

    if (IsReadable(a1, 256))
    {
        fwrite("  a1 first64 hex:", 1, 17, f);
        fwrite(a1, 1, 64, f);
        short s16[16] = {};
        memcpy(s16, a1, sizeof(s16));
        n = snprintf(head, sizeof(head),
                     "\n  a1 int16 x16: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                     s16[0], s16[1], s16[2], s16[3], s16[4], s16[5], s16[6], s16[7],
                     s16[8], s16[9], s16[10], s16[11], s16[12], s16[13], s16[14], s16[15]);
        if (n > 0)
            fwrite(head, 1, (size_t)n, f);
        float f32[8] = {};
        memcpy(f32, a1, sizeof(f32));
        n = snprintf(head, sizeof(head), "  a1 f32 x8: %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
                     f32[0], f32[1], f32[2], f32[3], f32[4], f32[5], f32[6], f32[7]);
        if (n > 0)
            fwrite(head, 1, (size_t)n, f);
    }
    if (IsReadable(a3, 32))
    {
        int i0 = 0;
        int i1 = 0;
        memcpy(&i0, (const unsigned char*)a3, 4);
        memcpy(&i1, (const unsigned char*)a3 + 4, 4);
        n = snprintf(head, sizeof(head), "  a3 ints: [0]=%d [4]=%d\n", i0, i1);
        if (n > 0)
            fwrite(head, 1, (size_t)n, f);
    }
    n = snprintf(head, sizeof(head), "  a2 int64=%lld low32=%d\n",
                 (long long)(intptr_t)a2, (int)(intptr_t)a2);
    if (n > 0)
        fwrite(head, 1, (size_t)n, f);
    fclose(f);
}

static void OnCaptureWrapper(void* a1, void* a2, void* a3,
                             double d0, double d1, double d2, double d3)
{
    if (g_shared)
        InterlockedOr((volatile LONG*)&g_shared->flags, MIO_ZEGO_FLAG_CAPTURE_INVOKED);

    const int len = (int)(intptr_t)a2;
    short before16[16] = {};
    bool have_before = false;
    if (a1 && len >= 32 && len <= (1 << 20) && IsReadable(a1, 32))
    {
        memcpy(before16, a1, sizeof(before16));
        have_before = true;
    }

    // 电平测量（增益前原始数据）→ Mio 监听页电平条
    if (g_level && a1 && len >= 2 && len <= (1 << 20) && (len & 1) == 0 && IsReadable(a1, (size_t)len))
    {
        const short* sp = (const short*)a1;
        const int cnt = len / 2;
        unsigned long long sum = 0;
        int peak = 0;
        for (int i = 0; i < cnt; ++i)
        {
            const int v = sp[i];
            const int av = v < 0 ? -v : v;
            if (av > peak)
                peak = av;
            sum += (unsigned long long)((long long)v * (long long)v);
        }
        unsigned int rm = 0;
        if (cnt > 0)
        {
            const double rms = sqrt((double)sum / (double)cnt);
            rm = (unsigned int)(rms * 1000.0 / 32768.0 + 0.5);
        }
        unsigned int pm = (unsigned int)((unsigned long long)peak * 1000 / 32768);
        if (rm > 1000)
            rm = 1000;
        if (pm > 1000)
            pm = 1000;
        g_level->seq++;
        g_level->rms_milli = rm;
        g_level->peak_milli = pm;
        g_level->tick_ms = GetTickCount64();
        g_level->seq++;
    }

    if (g_injector_alive)
        ApplyGainToCapture(a1, len); // 注入器退出后自动跳过（增益取消）
    g_boost_last_capture = GetTickCount64(); // 采集活跃标记（侧信息线程按此发）
    ForceCleanDsp(); // 净化：关掉即构自带 AGC/ANS（防和我们的增益打架）
    DumpCaptureArgs(a1, a2, a3, d0, d1, d2, d3, before16, have_before);

    if (g_orig_w_capture)
        g_orig_w_capture(a1, a2, a3, d0, d1, d2, d3);
}

static void OnCaptureRegistered(void* a1, void* a2, void* a3,
                                double d0, double d1, double d2, double d3)
{
    if (g_app_capture_cb)
        ((CaptureWrapperFn)g_app_capture_cb)(a1, a2, a3, d0, d1, d2, d3);
}

static void OnRegisterCapture(void* cb, void* user_data, void* a3, void* a4)
{
    g_app_capture_cb = cb;
    if (g_orig_register_capture)
        g_orig_register_capture(cb ? (void*)&OnCaptureRegistered : nullptr, user_data, a3, a4);
}

// ---------------------------------------------------------------------------
// 400kbps 强制套件（注入式，与码率显示同源；无 UI、无磁盘改动）
//   A) API 校验点：RVA 0xD56E4E 立即数 192→400
//      （41 81 3E C0 00 00 00  = cmp [r14], 192；错误串 "audio bitrate invalid"）
//   B) 192000(bps) 立即数升级 400000：扫描可执行段 cmp/mov 形态；
//      排除 lea 数学形态 与 0x4AFF（码率/10 换算）邻域、以及 cmp edx,imm;ja 带宽逻辑
//   C) 钩住 set_audio_config_by_channel：把配置结构 +0（bitrate）强制改 400
// ---------------------------------------------------------------------------

typedef int (*SetAudioCfgByChannelFn)(void* cfg, int channel);
static SetAudioCfgByChannelFn g_orig_setcfg_ch = nullptr;

static void AppendProbeLog(const wchar_t* name, const char* line)
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\%s-%lu.txt",
                 base, name, (unsigned long)GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int OnSetAudioConfigByChannel(void* cfg, int channel)
{
    if (cfg)
    {
        unsigned char local[16] = {};
        memcpy(local, cfg, 12);
        static volatile LONG call_no = 0;
        const LONG cn = InterlockedIncrement(&call_no);
        if (cn <= 20)
        {
            char line[192] = {};
            snprintf(line, sizeof(line),
                     "setcfg call#%ld ch=%d req: bitrate=%d channel=%d codec=%d -> force 400/6",
                     (long)cn, channel, *(int*)(local + 0), *(int*)(local + 4), *(int*)(local + 8));
            AppendProbeLog(L"bitrate400-calls", line);
        }
        *(int*)(local + 0) = 400; // +0 = bitrate(kbps)，强制 400
        *(int*)(local + 8) = 6;   // +8 = codecID，强制 6（Low3/Opus，400k 唯一活路）
        if (g_orig_setcfg_ch)
            return g_orig_setcfg_ch(local, channel);
        return 0;
    }
    if (g_orig_setcfg_ch)
        return g_orig_setcfg_ch(cfg, channel);
    return 0;
}

static bool PatchCodeBytes(void* addr, const unsigned char* bytes, size_t n)
{
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    return true;
}

struct Bitrate400Report
{
    int patch_a;
    int patch_b;
    bool hook_ok;
    int b_count;
    unsigned int b_addrs[64];
};

static void ApplyBitrate400Patches(HMODULE zegо, Bitrate400Report* rep)
{
    if (!zegо || !rep)
        return;
    const unsigned char* base = (const unsigned char*)zegо;

    // A：校验点指纹匹配后改立即数
    {
        unsigned char* site = (unsigned char*)base + 0xD56E4B;
        static const unsigned char sig[7] = { 0x41, 0x81, 0x3E, 0xC0, 0x00, 0x00, 0x00 };
        if (memcmp(site, sig, sizeof(sig)) == 0)
        {
            const unsigned char imm[4] = { 0x90, 0x01, 0x00, 0x00 }; // 400
            if (PatchCodeBytes(site + 3, imm, 4))
                rep->patch_a = 1;
        }
    }

    // A2：set_audio_config 真正校验点（错误码 1003002 来源）
    //     41 81 FE C0 00 00 00 = cmp r14d, 192 → 改 400
    {
        unsigned char* site = (unsigned char*)base + 0xD699D5;
        static const unsigned char sig2[7] = { 0x41, 0x81, 0xFE, 0xC0, 0x00, 0x00, 0x00 };
        if (memcmp(site, sig2, sizeof(sig2)) == 0)
        {
            const unsigned char imm[4] = { 0x90, 0x01, 0x00, 0x00 }; // 400
            if (PatchCodeBytes(site + 3, imm, 4))
                rep->patch_a++;
        }
    }

    // B：遍历可执行段，升级 192000 立即数
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int si = 0; si < nt->FileHeader.NumberOfSections; ++si)
        {
            if (!(sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            unsigned char* s = (unsigned char*)base + sec[si].VirtualAddress;
            const size_t n = sec[si].Misc.VirtualSize;
            if (n < 16)
                continue;
            for (size_t i = 6; i + 6 <= n; ++i)
            {
                if (!(s[i] == 0x00 && s[i + 1] == 0xEE && s[i + 2] == 0x02 && s[i + 3] == 0x00))
                    continue;
                bool hit = false;
                // mov r32, imm32（B8-BF）
                if (s[i - 1] >= 0xB8 && s[i - 1] <= 0xBF)
                    hit = true;
                // 41 B8-BF：mov r??d, imm32
                if (s[i - 2] == 0x41 && s[i - 1] >= 0xB8 && s[i - 1] <= 0xBF)
                    hit = true;
                // 81 /7 寄存器形态：cmp r32, imm32
                if (s[i - 2] == 0x81 && (s[i - 1] & 0xC0) == 0xC0 && (s[i - 1] & 0x38) == 0x38)
                    hit = true;
                // 41 81 /7：cmp r??d, imm32
                if (s[i - 3] == 0x41 && s[i - 2] == 0x81 && (s[i - 1] & 0xC0) == 0xC0 && (s[i - 1] & 0x38) == 0x38)
                    hit = true;
                // 81 /7 [mem+disp32]：cmp dword [reg+disp32], imm32
                if (s[i - 6] == 0x81 && ((s[i - 5] & 0xC0) == 0x40 || (s[i - 5] & 0xC0) == 0x80) && (s[i - 5] & 0x38) == 0x38)
                    hit = true;
                if (!hit)
                    continue;
                // 排除1：文档政策——cmp edx, imm; ja（带宽逻辑）
                if (s[i - 2] == 0x81 && s[i - 1] == 0xFA &&
                    (s[i + 4] == 0x77 || (s[i + 4] == 0x0F && s[i + 5] == 0x87)))
                    continue;
                // 排除2：后随 mov edx/ebx, 0x4AFF（码率/10 换算邻域）
                bool excl = false;
                for (size_t k = i + 4; k + 5 <= n && k <= i + 16; ++k)
                {
                    if ((s[k] == 0xBA || s[k] == 0xBB) &&
                        s[k + 1] == 0xFF && s[k + 2] == 0x4A && s[k + 3] == 0x00 && s[k + 4] == 0x00)
                    {
                        excl = true;
                        break;
                    }
                }
                if (excl)
                    continue;
                const unsigned char imm[4] = { 0x80, 0x1A, 0x06, 0x00 }; // 400000
                if (PatchCodeBytes(s + i, imm, 4))
                {
                    rep->patch_b++;
                    if (rep->b_count < 64)
                        rep->b_addrs[rep->b_count++] = (unsigned int)((uintptr_t)(s + i) - (uintptr_t)base);
                }
            }
        }
    }
}

static void WriteBitrate400Report(const Bitrate400Report* rep)
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe", base);
    CreateDirectoryW(path, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Mio\\zego-probe\\bitrate400-%lu.txt",
                 base, (unsigned long)GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f)
        return;
    fprintf(f, "bitrate400 patchA=%d patchB=%d hook=%d\n",
            rep->patch_a, rep->patch_b, rep->hook_ok ? 1 : 0);
    for (int i = 0; i < rep->b_count; ++i)
        fprintf(f, "  B @ RVA 0x%X\n", rep->b_addrs[i]);
    fclose(f);
}

// 自证：直呼 set_audio_config({400,2,6})，再 get_audio_config 回读
static bool Bitrate400SelfTest(HMODULE zegо)
{
    typedef int (*SetCfgFn)(void* cfg);
    typedef int (*GetCfgFn)(void* out);
    SetCfgFn fn_set = (SetCfgFn)GetProcAddress(zegо, "zego_express_set_audio_config");
    GetCfgFn fn_get = (GetCfgFn)GetProcAddress(zegо, "zego_express_get_audio_config");
    if (!fn_set || !fn_get)
        return false;

    static volatile LONG tries = 0;
    const LONG t = InterlockedIncrement(&tries);

    int cfg[4] = { 400, 2, 6, 0 };
    const int rc_set = fn_set(cfg);
    Sleep(150);
    int out[4] = { -1, -1, -1, -1 };
    const int rc_get = fn_get(out);

    if (t <= 40)
    {
        char line[224] = {};
        snprintf(line, sizeof(line),
                 "selftest#%ld set rc=%d (req 400/2/6) | get rc=%d -> bitrate=%d channel=%d codec=%d",
                 (long)t, rc_set, rc_get, out[0], out[1], out[2]);
        AppendProbeLog(L"bitrate400-selftest", line);
    }
    return rc_set == 0 && out[0] == 400;
}

// ---------------------------------------------------------------------------
// 工作线程

// 预载 ZegoExpressEngine.dll：让钩子在任何 createEngine/回调注册发生之前就位。
// 路径按 TT 安装布局拼接（<TT安装目录>\resources\app.asar.unpacked\
// node_modules\zego-express-engine-electron\win\x64\ZegoExpressEngine.dll）；
// 文件不存在则返回空，由轮询兜底。
static HMODULE PreloadZego()
{
    HMODULE existing = GetModuleHandleW(L"ZegoExpressEngine.dll");
    if (existing)
        return existing;

    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return nullptr;

    std::wstring path(exe);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return nullptr;
    path.resize(slash);
    path += L"\\resources\\app.asar.unpacked\\node_modules\\zego-express-engine-electron\\win\\x64\\ZegoExpressEngine.dll";

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return nullptr;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// ---------------------------------------------------------------------------
// 防闭麦：控制共享内存（Mio 写，本载荷读）+ 静音类导出拦截
//   zego_express_mute_microphone(bool)                  —— 闭麦
//   zego_express_enable_audio_capture_device(bool)      —— 停采集
//   zego_express_mute_publish_stream_audio(bool, int)   —— 停发音频
// 开启时：吞掉所有「闭麦/停采集/停发音频」请求；关闭时原样放行。
static HANDLE            g_ctrl_mapping = nullptr;
static MioControlShared* g_ctrl = nullptr;
static void EnsureControlShared()
{
    if (g_ctrl)
        return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_CONTROL_SHARED_NAME);
    if (!mapping)
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(MioControlShared), MIO_CONTROL_SHARED_NAME);
    if (!mapping)
        return;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioControlShared));
    if (!view)
    {
        CloseHandle(mapping);
        return;
    }
    g_ctrl_mapping = mapping;
    g_ctrl = (MioControlShared*)view;
    if (g_ctrl->magic != MIO_CONTROL_SHARED_MAGIC)
    {
        memset(g_ctrl, 0, sizeof(*g_ctrl));
        g_ctrl->magic = MIO_CONTROL_SHARED_MAGIC;
        g_ctrl->version = 1;
    }
}
static bool AntiMuteActive()
{
    if (!g_ctrl)
        EnsureControlShared();
    if (!g_ctrl || g_ctrl->magic != MIO_CONTROL_SHARED_MAGIC)
        return false;
    return (g_ctrl->flags & MIO_CTRL_FLAG_ANTI_MUTE) != 0;
}
// 注入器（Mio）心跳：>5 秒无打点视为已退出 → 增益自动取消
static bool InjectorAlive()
{
    if (!g_ctrl)
        EnsureControlShared();
    if (!g_ctrl || g_ctrl->magic != MIO_CONTROL_SHARED_MAGIC)
        return false;
    const uint64_t tick = g_ctrl->tick_ms;
    if (tick == 0)
        return false;
    return (GetTickCount64() - tick) <= 5000;
}
static void CountAntiMuteBlock()
{
    if (g_shared)
        InterlockedIncrement((volatile LONG*)&g_shared->anti_mute_blocked);
}
typedef void (*MuteMicFn)(bool mute);
static MuteMicFn g_orig_mute_mic = nullptr;
static void OnMuteMicrophone(bool mute)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_mic_calls);
    if (mute && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_mute_mic)
            g_orig_mute_mic(false); // 强行保持开麦（防.js 同款）
        return;
    }
    if (g_orig_mute_mic)
        g_orig_mute_mic(mute);
}
typedef void (*EnableCapFn)(bool enable);
static EnableCapFn g_orig_enable_cap = nullptr;
static void OnEnableAudioCaptureDevice(bool enable)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_cap_calls);
    if (!enable && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_enable_cap)
            g_orig_enable_cap(true); // 强行保持采集（防.js 同款）
        return;
    }
    if (g_orig_enable_cap)
        g_orig_enable_cap(enable);
}
typedef void (*MutePubAudFn)(bool mute, int channel);
static MutePubAudFn g_orig_mute_pub = nullptr;
static void OnMutePublishStreamAudio(bool mute, int channel)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_pub_calls);
    if (mute && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_mute_pub)
            g_orig_mute_pub(false, channel); // 强行保持发音频（防.js 同款）
        return;
    }
    if (g_orig_mute_pub)
        g_orig_mute_pub(mute, channel);
}
typedef void (*SetCapVolFn)(int volume);
static SetCapVolFn g_orig_set_cap_vol = nullptr;
static void OnSetCaptureVolume(int volume)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_vol_calls);
    if (volume == 0 && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_set_cap_vol)
            g_orig_set_cap_vol(100); // 音量归零强改 100（防.js 同款）
        return;
    }
    if (g_orig_set_cap_vol)
        g_orig_set_cap_vol(volume);
}
typedef void (*Raw4Fn)(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
static Raw4Fn g_orig_mute_dev = nullptr;
static void OnMuteAudioDevice(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_mad_calls);
    if ((a4 & 0xFF) != 0 && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_mute_dev)
            g_orig_mute_dev(a1, a2, a3, 0); // 强解设备静音（防.js 同款）
        return;
    }
    if (g_orig_mute_dev)
        g_orig_mute_dev(a1, a2, a3, a4);
}
static Raw4Fn g_orig_dev_vol = nullptr;
static void OnSetAudioDeviceVolume(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_devvol_calls);
    if (g_orig_dev_vol)
        g_orig_dev_vol(a1, a2, a3, a4);
}
static Raw4Fn g_orig_start_pub = nullptr;
static void OnStartPublishingStream(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_startpub_calls);
    if (g_orig_start_pub)
        g_orig_start_pub(a1, a2, a3, a4);
}
static Raw4Fn g_orig_stop_pub = nullptr;
static void OnStopPublishingStream(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    EnsureControlShared();
    if (g_ctrl)
        InterlockedIncrement((volatile LONG*)&g_ctrl->am_stoppub_calls);
    if (AntiMuteActive())
    {
        CountAntiMuteBlock();
        return; // 拦截停止推流（防.js 同款：被闭麦也保持推流）
    }
    if (g_orig_stop_pub)
        g_orig_stop_pub(a1, a2, a3, a4);
}
typedef void (*MuteSpeakerFn)(bool mute);
static MuteSpeakerFn g_orig_mute_spk = nullptr;
static void OnMuteSpeaker(bool mute)
{
    if (mute && AntiMuteActive())
    {
        CountAntiMuteBlock();
        if (g_orig_mute_spk)
            g_orig_mute_spk(false); // 保持能听（防.js 同款）
        return;
    }
    if (g_orig_mute_spk)
        g_orig_mute_spk(mute);
}
// ---------------------------------------------------------------------------

static DWORD WINAPI WorkerProc(LPVOID)
{
    if (!InitializeCriticalSectionAndSpinCount(&g_lock, 4000))
        return 0;
    g_lock_ready = true;

    EnsureShared();
    EnsureLevelShared();

    // 第一步：预载（抢占挂钩，彻底避开"注册早于注入"的竞态）
    HMODULE zego = PreloadZego();
    if (zego && g_shared)
        g_shared->flags |= MIO_ZEGO_FLAG_ZEGO_FOUND | MIO_ZEGO_FLAG_PRELOADED;

    // 第二步：未预载成功则 100ms 粒度轮询等待模块出现（最多约 15 分钟）
    for (int i = 0; !zego && i < 9000; ++i)
    {
        zego = GetModuleHandleW(L"ZegoExpressEngine.dll");
        if (!zego)
            Sleep(100);
    }

    if (!zego)
    {
        if (g_shared)
            g_shared->hook_error = MIO_ZEGO_HOOK_NO_ZEGO_DLL;
        for (;;)
            Sleep(5000);
    }

    if (g_shared)
        g_shared->flags |= MIO_ZEGO_FLAG_ZEGO_FOUND;

    void* target_play = (void*)GetProcAddress(zego, "zego_register_player_quality_update_callback");
    void* target_pub = (void*)GetProcAddress(zego, "zego_register_publisher_quality_update_callback");
    void* target_stream = (void*)GetProcAddress(zego, "zego_register_room_stream_update_callback");
    if (!target_play || !target_pub)
    {
        if (g_shared)
            g_shared->hook_error = MIO_ZEGO_HOOK_EXPORT_MISSING;
        for (;;)
            Sleep(5000);
    }

    if (MH_Initialize() != MH_OK)
    {
        if (g_shared)
            g_shared->hook_error = MIO_ZEGO_HOOK_MH_INIT_FAIL;
        for (;;)
            Sleep(5000);
    }

    bool ok_play = false;
    if (MH_CreateHook(target_play, (LPVOID)&OnRegisterPlay, (LPVOID*)&g_orig_register_play) == MH_OK &&
        MH_EnableHook(target_play) == MH_OK)
        ok_play = true;

    bool ok_pub = false;
    if (MH_CreateHook(target_pub, (LPVOID)&OnRegisterPub, (LPVOID*)&g_orig_register_pub) == MH_OK &&
        MH_EnableHook(target_pub) == MH_OK)
        ok_pub = true;

    bool ok_stream = false;
    if (target_stream &&
        MH_CreateHook(target_stream, (LPVOID)&OnRegisterStream, (LPVOID*)&g_orig_register_stream) == MH_OK &&
        MH_EnableHook(target_stream) == MH_OK)
        ok_stream = true;

    // 附加：挂钩组件流块转换器（与注册时机无关的兜底取名 + 原始块转储）
    HMODULE node_mod = GetModuleHandleW(L"ZegoExpressNodeNative.node");
    for (int i = 0; !node_mod && i < 600; ++i)
    {
        Sleep(200);
        node_mod = GetModuleHandleW(L"ZegoExpressNodeNative.node");
    }
    bool ok_conv = false;
    if (node_mod)
    {
        void* conv = (unsigned char*)node_mod + 0x3CC0; // 3.21.0-45645.1188 反汇实测 RVA
        if (MH_CreateHook(conv, (LPVOID)&OnStreamConvert, (LPVOID*)&g_orig_conv) == MH_OK &&
            MH_EnableHook(conv) == MH_OK)
            ok_conv = true;
    }

    // 兜底直挂三个事件包装函数（与注册时机无关，彻底消除竞态）
    bool ok_wrap = false;
    if (node_mod)
    {
        void* w_play = (unsigned char*)node_mod + 0x1D970;   // 拉流质量包装
        void* w_pub = (unsigned char*)node_mod + 0x1B500;    // 推流质量包装
        void* w_stream = (unsigned char*)node_mod + 0x19540; // 房间流列表包装
        const bool a = MH_CreateHook(w_play, (LPVOID)&OnPlayQualityWrapper, (LPVOID*)&g_orig_w_play) == MH_OK &&
                       MH_EnableHook(w_play) == MH_OK;
        const bool b = MH_CreateHook(w_pub, (LPVOID)&OnPubQualityWrapper, (LPVOID*)&g_orig_w_pub) == MH_OK &&
                       MH_EnableHook(w_pub) == MH_OK;
        const bool c = MH_CreateHook(w_stream, (LPVOID)&OnStreamUpdateWrapper, (LPVOID*)&g_orig_w_stream) == MH_OK &&
                       MH_EnableHook(w_stream) == MH_OK;
        ok_wrap = a && b && c;
    }

    // 采集处理回调直挂（增益注入点）+ 其注册函数转发链
    bool ok_cap = false;
    if (node_mod)
    {
        void* w_cap = (unsigned char*)node_mod + 0x26B40; // 采集处理包装（3.21.0 反汇实测）
        ok_cap = MH_CreateHook(w_cap, (LPVOID)&OnCaptureWrapper, (LPVOID*)&g_orig_w_capture) == MH_OK &&
                 MH_EnableHook(w_cap) == MH_OK;
    }
    void* target_capture = (void*)GetProcAddress(zego, "zego_register_process_captured_audio_data_callback");
    if (target_capture &&
        MH_CreateHook(target_capture, (LPVOID)&OnRegisterCapture, (LPVOID*)&g_orig_register_capture) == MH_OK)
        MH_EnableHook(target_capture);

    // 400kbps 强制套件：先打内存补丁，再钩强制器
    Bitrate400Report bt_rep = {};
    ApplyBitrate400Patches(zego, &bt_rep);
    void* p_setcfg_ch = (void*)GetProcAddress(zego, "zego_express_set_audio_config_by_channel");
    if (p_setcfg_ch &&
        MH_CreateHook(p_setcfg_ch, (LPVOID)&OnSetAudioConfigByChannel, (LPVOID*)&g_orig_setcfg_ch) == MH_OK &&
        MH_EnableHook(p_setcfg_ch) == MH_OK)
        bt_rep.hook_ok = true;
    WriteBitrate400Report(&bt_rep);

    // 防闭麦套件：拦静音/停采集类导出（开关状态由 Mio 写控制共享内存）
    EnsureControlShared();
    bool ok_am_mic = false;
    void* p_mute_mic = (void*)GetProcAddress(zego, "zego_express_mute_microphone");
    if (p_mute_mic &&
        MH_CreateHook(p_mute_mic, (LPVOID)&OnMuteMicrophone, (LPVOID*)&g_orig_mute_mic) == MH_OK &&
        MH_EnableHook(p_mute_mic) == MH_OK)
        ok_am_mic = true;
    bool ok_am_cap = false;
    void* p_en_cap = (void*)GetProcAddress(zego, "zego_express_enable_audio_capture_device");
    if (p_en_cap &&
        MH_CreateHook(p_en_cap, (LPVOID)&OnEnableAudioCaptureDevice, (LPVOID*)&g_orig_enable_cap) == MH_OK &&
        MH_EnableHook(p_en_cap) == MH_OK)
        ok_am_cap = true;
    bool ok_am_pub = false;
    void* p_mute_pub = (void*)GetProcAddress(zego, "zego_express_mute_publish_stream_audio");
    if (p_mute_pub &&
        MH_CreateHook(p_mute_pub, (LPVOID)&OnMutePublishStreamAudio, (LPVOID*)&g_orig_mute_pub) == MH_OK &&
        MH_EnableHook(p_mute_pub) == MH_OK)
        ok_am_pub = true;
    bool ok_am_vol = false;
    void* p_cap_vol = (void*)GetProcAddress(zego, "zego_express_set_capture_volume");
    if (p_cap_vol &&
        MH_CreateHook(p_cap_vol, (LPVOID)&OnSetCaptureVolume, (LPVOID*)&g_orig_set_cap_vol) == MH_OK &&
        MH_EnableHook(p_cap_vol) == MH_OK)
        ok_am_vol = true;
    void* p_mute_dev = (void*)GetProcAddress(zego, "zego_express_mute_audio_device");
    if (p_mute_dev)
        MH_CreateHook(p_mute_dev, (LPVOID)&OnMuteAudioDevice, (LPVOID*)&g_orig_mute_dev) == MH_OK &&
        MH_EnableHook(p_mute_dev);
    void* p_dev_vol = (void*)GetProcAddress(zego, "zego_express_set_audio_device_volume");
    if (p_dev_vol)
        MH_CreateHook(p_dev_vol, (LPVOID)&OnSetAudioDeviceVolume, (LPVOID*)&g_orig_dev_vol) == MH_OK &&
        MH_EnableHook(p_dev_vol);
    void* p_start_pub = (void*)GetProcAddress(zego, "zego_express_start_publishing_stream");
    if (p_start_pub)
        MH_CreateHook(p_start_pub, (LPVOID)&OnStartPublishingStream, (LPVOID*)&g_orig_start_pub) == MH_OK &&
        MH_EnableHook(p_start_pub);
    void* p_stop_pub = (void*)GetProcAddress(zego, "zego_express_stop_publishing_stream");
    if (p_stop_pub)
        MH_CreateHook(p_stop_pub, (LPVOID)&OnStopPublishingStream, (LPVOID*)&g_orig_stop_pub) == MH_OK &&
        MH_EnableHook(p_stop_pub);
    void* p_mute_spk = (void*)GetProcAddress(zego, "zego_express_mute_speaker");
    if (p_mute_spk)
        MH_CreateHook(p_mute_spk, (LPVOID)&OnMuteSpeaker, (LPVOID*)&g_orig_mute_spk) == MH_OK &&
        MH_EnableHook(p_mute_spk);
    // 4000kbps 侧信息加持：解析导出 + 按目标重建包组
    g_fn_side_info = (SendAudioSideInfoFn)GetProcAddress(zego, "zego_express_send_audio_side_info");
    g_fn_set_engine_config = (SetEngineConfigFn)GetProcAddress(zego, "zego_express_set_engine_config");
    void* p_agc = (void*)GetProcAddress(zego, "zego_express_enable_agc");
    if (p_agc)
        MH_CreateHook(p_agc, (LPVOID)&OnEnableAgc, (LPVOID*)&g_orig_enable_agc) == MH_OK && MH_EnableHook(p_agc);
    void* p_ans = (void*)GetProcAddress(zego, "zego_express_enable_ans");
    if (p_ans)
        MH_CreateHook(p_ans, (LPVOID)&OnEnableAns, (LPVOID*)&g_orig_enable_ans) == MH_OK && MH_EnableHook(p_ans);
    void* p_tans = (void*)GetProcAddress(zego, "zego_express_enable_transient_ans");
    if (p_tans)
        MH_CreateHook(p_tans, (LPVOID)&OnEnableTans, (LPVOID*)&g_orig_enable_tans) == MH_OK && MH_EnableHook(p_tans);
    void* p_sph = (void*)GetProcAddress(zego, "zego_express_enable_speech_enhance");
    if (p_sph)
        MH_CreateHook(p_sph, (LPVOID)&OnEnableSphenh, (LPVOID*)&g_orig_enable_sphenh) == MH_OK && MH_EnableHook(p_sph);
    BoostRebuildChunks((int)g_boost_target, 400);
    BoostPublishToShared();
    if (g_fn_side_info)
    {
        char line[128] = {};
        snprintf(line, sizeof(line), "side-info ready target=%ldkbps chunks=%d bytes/frame=%ld",
                 (long)g_boost_target, (int)g_boost_count, (long)g_boost_bytes);
        BoostLogFirst(line);
        HANDLE bt = ::CreateThread(nullptr, 0, BoostThreadProc, nullptr, 0, nullptr);
        if (bt)
            ::CloseHandle(bt);
    }

    if (g_shared)
    {
        if (ok_play)
            g_shared->flags |= MIO_ZEGO_FLAG_PLAY_HOOKED;
        if (ok_pub)
            g_shared->flags |= MIO_ZEGO_FLAG_PUB_HOOKED;
        if (ok_stream)
            g_shared->flags |= MIO_ZEGO_FLAG_STREAM_HOOKED;
        if (ok_conv)
            g_shared->flags |= MIO_ZEGO_FLAG_CONV_HOOKED;
        if (ok_wrap)
            g_shared->flags |= MIO_ZEGO_FLAG_WRAP_HOOKED;
        if (ok_cap)
            g_shared->flags |= MIO_ZEGO_FLAG_CAPTURE_HOOKED;
        if (bt_rep.patch_a)
            g_shared->flags |= MIO_ZEGO_FLAG_BITRATE400;
        if (ok_am_mic)
            g_shared->flags |= MIO_ZEGO_FLAG_ANTI_MUTE_HOOKED;
        if (g_fn_side_info)
            g_shared->flags |= MIO_ZEGO_FLAG_SIDE_INFO_ACTIVE;
        if (!ok_play || !ok_pub)
            g_shared->hook_error = (!ok_play && !ok_pub) ? MIO_ZEGO_HOOK_CREATE_FAIL : MIO_ZEGO_HOOK_ENABLE_FAIL;
    }

    // 常驻：400 自证重试（15s 粒度）+ 防闭麦状态轮询（250ms 粒度，开启瞬间强制解除已有闭麦）
    static volatile LONG bt400_ok = 0;
    static LONG anti_last = -1;
    int heartbeat = 60;
    for (;;)
    {
        EnsureControlShared();
        const bool alive = InjectorAlive();
        InterlockedExchange(&g_injector_alive, alive ? 1 : 0);
        if (g_shared)
        {
            if (alive)
                InterlockedOr((volatile LONG*)&g_shared->flags, MIO_ZEGO_FLAG_INJECTOR_ALIVE);
            else
                InterlockedAnd((volatile LONG*)&g_shared->flags, ~MIO_ZEGO_FLAG_INJECTOR_ALIVE);
        }
        const LONG anti_now = AntiMuteActive() ? 1 : 0;
        if (anti_now != anti_last)
        {
            const bool rising = (anti_now == 1);
            anti_last = anti_now;
            if (rising)
            {
                if (g_orig_mute_mic)
                    g_orig_mute_mic(false);
                if (g_orig_enable_cap)
                    g_orig_enable_cap(true);
                if (g_orig_mute_pub)
                    g_orig_mute_pub(false, 0);
            }
        }
        if (++heartbeat >= 60)
        {
            heartbeat = 0;
            if (!bt400_ok && Bitrate400SelfTest(zego))
                bt400_ok = 1;
        }
        {
            // 1 秒粒度：取走窗口底流基准 → 按【目标 − 底流】动态重建侧信息包组 → 回写共享内存
            static int boost_cfg_tick = 0;
            static int base_smooth = 400;
            if (++boost_cfg_tick >= 4)
            {
                boost_cfg_tick = 0;
                const int t = ReadBoostTargetFile();
                const int base_raw = (int)InterlockedExchange((volatile LONG*)&g_win_pub_kbps, 0);
                if (base_raw > 0)
                    base_smooth = base_raw; // 本秒有底流样本：对齐
                else if (base_smooth > 1)
                    base_smooth /= 2;       // 无样本：向静音衰减
                if (t == 0 || (t >= 200 && t <= 20000))
                {
                    g_boost_target = t;
                    BoostRebuildChunks(t, base_smooth);
                    BoostPublishToShared();
                }
            }
        }
        Sleep(250);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
