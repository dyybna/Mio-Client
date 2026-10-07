#pragma once
// ---------------------------------------------------------------------------
// MioJamShared.h —— Mio 与 MioJammer.dll（注入载荷）之间的控制块契约
//   两块代码必须用同一份定义。名字/魔数刻意与原版 TT语音OpusJammer 区分开，
//   避免和原外挂的 Local\TTVoiceOpusJammer_Ctl_v1 打架。
// ---------------------------------------------------------------------------
#include <cstdint>

#define MIO_JAM_SHARED_NAME   L"Local\\MioJammer_Ctl_v1"
#define MIO_JAM_MAGIC         0x4D4A4D4Au   // 'M''J''M''J'  (LE)

// 干扰模式（与原外挂的面板下拉一一对应）
enum : uint32_t
{
    MIO_JAM_MODE_AUTO   = 0,   // 自动(RTP+盲毁)
    MIO_JAM_MODE_RTP    = 1,   // 仅RTP(明文)：只砸真 RTP，其余放行
    MIO_JAM_MODE_BLIND  = 2,   // 仅盲毁(密文)
    MIO_JAM_MODE_CRASH3 = 3,   // 崩溃：TCP 长包强改
    MIO_JAM_MODE_CRASH4 = 4,   // 崩溃：长 TCP 改 + 丢特征 UDP
    MIO_JAM_MODE_COUNT  = 5,
};

// 控制块（0xA8 = 168 字节，与原外挂布局保持同构，方便互相移植）
#pragma pack(push, 1)
struct MioJamShared
{
    uint32_t magic;         // +0x00  MIO_JAM_MAGIC（DLL 初始化时写）
    uint32_t jamSwitch;     // +0x04  双向：Mio 写开关，DLL 读
    uint32_t reqUnload;     // +0x08  Mio 写非 0 -> 所有已注入进程自我卸载
    uint32_t jamEnabled;    // +0x0C  DLL 回读
    uint32_t initialized;   // +0x10  DLL 写 1
    uint32_t logEnabled;    // +0x14  DLL 镜像
    uint32_t mode;          // +0x18  Mio 写模式
    uint32_t payloadType;   // +0x1C  Mio 写 RTP PT（0 = 不过滤）
    uint32_t zero20;        // +0x20  DLL 恒 0
    uint32_t pid;           // +0x24  DLL 写宿主 PID（多进程时会被最后一个覆盖）
    wchar_t  exeName[0x3F]; // +0x28  DLL 写宿主 exe 名
    uint16_t tail;          // +0xA6
};
#pragma pack(pop)

static_assert(sizeof(MioJamShared) == 0xA8, "MioJamShared layout changed");


// ---------------------------------------------------------------------------
// 统计块（独立映射）：让 Mio 能看见"到底拦到包没有"——这是排障的关键
//   每个已注入进程占一个 slot（按 pid），Mio 汇总读取。
// ---------------------------------------------------------------------------
#define MIO_JAM_STATS_NAME   L"Local\\MioJammer_Stats_v1"
#define MIO_JAM_STATS_MAGIC  0x534A4D4Du   // 'M''J''M''S'
#define MIO_JAM_STAT_SLOTS   32

#pragma pack(push, 1)
struct MioJamStatSlot
{
    uint32_t pid;        // 0 = 空槽
    uint32_t seen;       // 进入 jam_transform 的包数
    uint32_t rtp;        // 命中 RTP 并砸掉的包数
    uint32_t blind;      // 盲毁命中数
    uint32_t dropped;    // 静默丢弃数
    uint32_t reserved;
    uint64_t lastMs;     // 最近一次命中时刻(GetTickCount64)
};
struct MioJamStats
{
    uint32_t magic;      // MIO_JAM_STATS_MAGIC
    uint32_t slotCount;
    MioJamStatSlot slots[MIO_JAM_STAT_SLOTS];
};
#pragma pack(pop)
