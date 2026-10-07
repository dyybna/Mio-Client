#pragma once
// ---------------------------------------------------------------------------
// MioBassMini.h —— BASS 2.4 的"最小声明"
//
// 为什么不直接用 SDK 的 bass.h：
//   本项目对 BASS 的**所有函数都走 GetProcAddress 动态取**（见 MioGain.h），
//   编译期只需要类型与常量；而 bass.dll 已经内嵌进 exe（BassBlob.h）。
//   因此不再依赖 BASS SDK 目录，工程完全自包含。
//
// 以下定义均按 BASS 2.4 官方头逐字对齐（字节布局一致，勿随意改动）。
// ---------------------------------------------------------------------------
#include <windows.h>

typedef DWORD HSTREAM;   // sample stream handle
typedef DWORD HRECORD;   // recording handle
typedef DWORD HSAMPLE;   // sample handle
typedef DWORD HPLUGIN;   // plugin handle

// 设备信息（输出/录音通用）
typedef struct {
    const char* name;    // description
    const char* driver;  // driver
    DWORD       flags;
} BASS_DEVICEINFO;

#define BASS_DEVICE_ENABLED   1
#define BASS_DEVICE_DEFAULT   2
#define BASS_DEVICE_INIT      4
#define BASS_DEVICE_LOOPBACK  8   // 录音设备里的"环回"（WASAPI loopback）

// 录音设备信息
typedef struct {
    DWORD flags;
    DWORD formats;
    DWORD inputs;
    BOOL  singlein;
    DWORD freq;
} BASS_RECORDINFO;

// 通道信息
typedef struct {
    DWORD       freq;
    DWORD       chans;
    DWORD       flags;
    DWORD       ctype;
    DWORD       origres;
    HPLUGIN     plugin;
    HSAMPLE     sample;
    const char* filename;
} BASS_CHANNELINFO;

// 回调
typedef DWORD (CALLBACK STREAMPROC)(HSTREAM handle, void* buffer, DWORD length, void* user);
typedef BOOL  (CALLBACK RECORDPROC)(HRECORD handle, const void* buffer, DWORD length, void* user);

#define STREAMPROC_PUSH  ((STREAMPROC*)-1)   // 推流模式（BASS_StreamPutData 喂数据）

// 其它用到的常量
#define BASS_SAMPLE_FLOAT  0x100   // 32bit float
#define BASS_ATTRIB_VOL    2       // 音量属性
#define BASS_ACTIVE_PLAYING 1
