#pragma once

// ---------------------------------------------------------------------------
// MioZegoProbeShared.h
// MioZegoProbe.dll（注入 TT renderer 的探针载荷）与 MioImGui.exe 之间的
// 命名共享内存布局。两边必须使用同一份定义。
//
// 数据来源：钩住 ZegoExpressEngine.dll 导出的
//   zego_register_player_quality_update_callback
//   zego_register_publisher_quality_update_callback
//   zego_register_room_stream_update_callback   （成员昵称映射：streamID -> userName）
// 在回调里读取每个人的实时音频码率（audioKBPS）。
//
// 结构体偏移依据 ZegoExpressNodeNative.node（zego 3.21.0）反汇实测：
//   拉流 zego_play_stream_quality     .audioKBPS @ +0x50 (double), .rtt @ +0x68 (int)
//   推流 zego_publish_stream_quality  .audioKBPS @ +0x30 (double), .rtt @ +0x38 (int)
//   流列表回调块（步长 0x640）：stream_id char[64] @ +0x00 / user_id char[256] @ +0x40
//                               / user_name char[256] @ +0x140 / extended char[1024] @ +0x240
// 读取前均做内存可读性校验 + 数值范围钳制，异常计数走 bad_struct_count。
// ---------------------------------------------------------------------------

#include <cstdint>

#define MIO_ZEGO_SHARED_NAME    L"Local\\MioZegoKbps_v1"
#define MIO_ZEGO_SHARED_MAGIC   0x424B5A4Du  // 'M''Z''K''B' (LE)
#define MIO_ZEGO_SHARED_VERSION 1u

#define MIO_ZEGO_MAX_STREAMS    48
#define MIO_ZEGO_STREAM_ID_MAX  96
#define MIO_ZEGO_NAME_MAX       64

// ---- 音频电平：独立小共享内存（供 Mio 监听页电平条，与上面结构零耦合）----
#define MIO_LEVEL_SHARED_NAME   L"Local\\MioAudioLevel_v1"
#define MIO_LEVEL_SHARED_MAGIC  0x564C414Du  // 'M''A''L''V' (LE)

// ---- 防闭麦控制：Mio 写、载荷读（与上面两块零耦合）----
#define MIO_CONTROL_SHARED_NAME  L"Local\\MioControl_v2"
#define MIO_CONTROL_SHARED_MAGIC 0x4C54434Du // 'M''C''T''L' (LE)
#define MIO_CTRL_FLAG_ANTI_MUTE  1u          // 位0：防闭麦开关

struct MioAudioLevelShared
{
    uint32_t magic;        // MIO_LEVEL_SHARED_MAGIC
    uint32_t version;      // 1
    uint32_t seq;          // 奇数=写入中
    uint32_t rms_milli;    // RMS 电平 0..1000（采集原始数据，增益前）
    uint32_t peak_milli;   // 峰值电平 0..1000
    uint32_t reserved;
    uint64_t tick_ms;      // 最近更新（GetTickCount64）
};

// 防闭麦控制块（Mio 写、载荷读；位定义见 MIO_CTRL_FLAG_*）
struct MioControlShared
{
    uint32_t magic;        // MIO_CONTROL_SHARED_MAGIC
    uint32_t version;      // 1
    uint32_t flags;        // MIO_CTRL_FLAG_*
    uint32_t reserved;
    uint64_t tick_ms;      // 最近更新（GetTickCount64）
    uint32_t am_mic_calls; // 诊断（载荷写）：拦到的静音链调用次数
    uint32_t am_cap_calls;
    uint32_t am_pub_calls;
    uint32_t am_vol_calls;
    uint32_t am_mad_calls;      // mute_audio_device
    uint32_t am_devvol_calls;   // set_audio_device_volume
    uint32_t am_startpub_calls; // start_publishing_stream
    uint32_t am_stoppub_calls;  // stop_publishing_stream
};

// 过期阈值（毫秒）：超过该时长没有新回调的条目视为陈旧
#define MIO_ZEGO_STALE_MS       12000u

enum : uint32_t
{
    MIO_ZEGO_DIR_PLAY    = 0,  // 拉流（房间内其他人的语音流）
    MIO_ZEGO_DIR_PUBLISH = 1,  // 推流（自己）
};

enum : uint32_t
{
    MIO_ZEGO_FLAG_DLL_READY    = 1u << 0,  // 载荷已加载并创建共享内存
    MIO_ZEGO_FLAG_ZEGO_FOUND   = 1u << 1,  // 已找到 ZegoExpressEngine.dll
    MIO_ZEGO_FLAG_PLAY_HOOKED  = 1u << 2,  // 拉流注册函数已挂钩
    MIO_ZEGO_FLAG_PUB_HOOKED   = 1u << 3,  // 推流注册函数已挂钩
    MIO_ZEGO_FLAG_PLAY_INVOKED = 1u << 4,  // 已收到拉流质量回调
    MIO_ZEGO_FLAG_PUB_INVOKED  = 1u << 5,  // 已收到推流质量回调
    MIO_ZEGO_FLAG_PRELOADED    = 1u << 6,  // 探针主动预载了 Zego DLL（抢占挂钩）
    MIO_ZEGO_FLAG_STREAM_HOOKED  = 1u << 7, // 流列表注册函数已挂钩（昵称来源）
    MIO_ZEGO_FLAG_STREAM_INVOKED = 1u << 8, // 已收到流列表回调
    MIO_ZEGO_FLAG_CONV_HOOKED    = 1u << 9, // 组件流块转换器已挂钩（原始转储/兜底取名）
    MIO_ZEGO_FLAG_WRAP_HOOKED    = 1u << 10, // 三个事件包装函数已直挂（免注册竞态）
    MIO_ZEGO_FLAG_CAPTURE_HOOKED  = 1u << 11, // 采集处理回调已挂钩（增益注入点）
    MIO_ZEGO_FLAG_CAPTURE_INVOKED = 1u << 12, // 采集处理回调已被调用
    MIO_ZEGO_FLAG_BITRATE400      = 1u << 13, // 400kbps 强制套件已应用（校验点已改）
    MIO_ZEGO_FLAG_ANTI_MUTE_HOOKED = 1u << 14, // 防闭麦：静音类导出已挂钩
    MIO_ZEGO_FLAG_INJECTOR_ALIVE = 1u << 15,    // 注入器心跳在线（超时则该位清除、增益自动取消）
    MIO_ZEGO_FLAG_SIDE_INFO_ACTIVE = 1u << 16,  // 侧信息加持（4000kbps）已上线
};

enum : uint32_t
{
    MIO_ZEGO_HOOK_OK            = 0,
    MIO_ZEGO_HOOK_NO_ZEGO_DLL   = 1,  // 等待超时也未找到 ZegoExpressEngine.dll
    MIO_ZEGO_HOOK_EXPORT_MISSING = 2, // 导出函数缺失
    MIO_ZEGO_HOOK_MH_INIT_FAIL  = 3,
    MIO_ZEGO_HOOK_CREATE_FAIL   = 4,
    MIO_ZEGO_HOOK_ENABLE_FAIL   = 5,
};

struct MioZegoStreamEntry
{
    char     stream_id[MIO_ZEGO_STREAM_ID_MAX]; // UTF-8/ASCII，null 结尾
    char     user_name[MIO_ZEGO_NAME_MAX];      // 成员昵称（来自流列表回调），UTF-8
    double   audio_kbps;    // audioKBPS，kbps
    double   video_kbps;    // videoKBPS（语音房一般为 0）
    int32_t  rtt_ms;        // rtt，毫秒（0 = 未知）
    uint32_t direction;     // MIO_ZEGO_DIR_*
    uint32_t seen_count;    // 收到质量回调次数
    uint32_t reserved0;
    uint64_t last_seen_ms;  // 最近回调时刻（GetTickCount64，载荷进程时钟）
};

struct MioZegoShared
{
    uint32_t magic;                 // MIO_ZEGO_SHARED_MAGIC
    uint32_t version;               // MIO_ZEGO_SHARED_VERSION
    uint32_t seq;                   // 写锁：奇数=写入中，偶数=稳定
    uint32_t flags;                 // MIO_ZEGO_FLAG_*
    uint32_t hook_error;            // MIO_ZEGO_HOOK_*
    uint32_t pid;                   // 载荷所在进程 PID
    uint32_t register_player_calls; // 拉流注册函数被调用次数
    uint32_t register_pub_calls;    // 推流注册函数被调用次数
    uint32_t register_stream_calls; // 流列表注册函数被调用次数
    uint32_t play_cb_count;         // 拉流质量回调总次数
    uint32_t pub_cb_count;          // 推流质量回调总次数
    uint32_t bad_struct_count;      // 结构体越界/数值异常次数
    uint32_t entry_count;           // 当前有效条目数
    uint32_t anti_mute_blocked;     // 防闭麦拦截次数（吞掉的闭麦请求）
    uint64_t payload_boot_ms;       // 载荷启动时刻（GetTickCount64）
    uint64_t reserved0;
    MioZegoStreamEntry entries[MIO_ZEGO_MAX_STREAMS];
};

static_assert(sizeof(MioZegoStreamEntry) == 200, "MioZegoStreamEntry layout changed");
