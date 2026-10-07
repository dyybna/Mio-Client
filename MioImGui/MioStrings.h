#pragma once
// ===========================================================================
// MioStrings.h —— 程序文字防篡改（自动生成，勿手改）
//   界面/登录页文案全部编译期 XOR 加密：二进制里没有明文，静态搜串搜不到、
//   直接改字符串也改不出正确内容；每条串附明文 CRC32，运行时解密后自校验。
//   生成器: work/gen_miostr.py   密钥: K1=0x5A17C3 K2=0x9E3779B9
// ===========================================================================
#include <string>
#include <cstring>

namespace mio_str {

enum Id {
    S_TITLE = 0,
    S_LOGIN_LABEL = 1,
    S_YES = 2,
    S_NO = 3,
    S_ABOUT = 4,
    S_TAB0 = 5,
    S_TAB1 = 6,
    S_TAB2 = 7,
    S_TAB3 = 8,
    S_EXIT = 9,
    S_ANTIMUTE = 10,
    S_DRIVER = 11,
    S_LEVEL = 12,
    S_RUNTIME = 13,
    S_TAMPER = 14,
    ID_COUNT = 15
};

// ---- 单字节流密钥（与生成器同式）----
inline unsigned char Key(unsigned i)
{
    unsigned k = 0x5A17C3u ^ (0x9E3779B9u * (i + 1u));
    return (unsigned char)((k & 0xFFu) ^ ((k >> 16) & 0xFFu));
}

// S_TITLE
static const unsigned char kE0[10] = { 0x5a,0xec,0x7b,0x80,0x52,0xef,0x7b,0x8f,0x85,0xfd };
// S_LOGIN_LABEL
static const unsigned char kE1[22] = { 0x47,0xe9,0x71,0xc1,0x62,0xe6,0x32,0xa3,0x85,0xf9,0x7d,0xd8,0x0d,0xce,0x7b,0x0a,0x17,0x5d,0x7f,0xd7,0x5c,0xde };
// S_YES
static const unsigned char kE2[6] = { 0x4e,0xe0,0x67,0x88,0x42,0xaa };
// S_NO
static const unsigned char kE3[5] = { 0x59,0xea,0x3c,0xe3,0x38 };
// S_ABOUT
static const unsigned char kE4[13] = { 0xf2,0x39,0x94,0x45,0x9e,0x12,0xfa,0x6a,0x6e,0xf5,0x4c,0xd5,0x57 };
// S_TAB0
static const unsigned char kE5[6] = { 0xf0,0x0f,0xa2,0x46,0x91,0x02 };
// S_TAB1
static const unsigned char kE6[6] = { 0xf0,0x1e,0x85,0x45,0x81,0x2f };
// S_TAB2
static const unsigned char kE7[6] = { 0xf2,0x00,0xa7,0x44,0xab,0x0d };
// S_TAB3
static const unsigned char kE8[12] = { 0xf1,0x0d,0xab,0x49,0x86,0x37,0xf5,0x4a,0x6a,0x6e,0x86,0x2b };
// S_EXIT
static const unsigned char kE9[6] = { 0xfe,0x05,0x94,0x45,0x96,0x39 };
// S_ANTIMUTE
static const unsigned char kE10[9] = { 0xfe,0x1d,0xa6,0x49,0x86,0x2e,0xfb,0x50,0x4d };
// S_DRIVER
static const unsigned char kE11[21] = { 0xf2,0x26,0xa4,0x45,0x9c,0x22,0xf4,0x4b,0x4e,0x6f,0x86,0x09,0xc4,0x26,0xbf,0x9b,0xf5,0xd5,0xd3,0x04,0xa2 };
// S_LEVEL
static const unsigned char kE12[21] = { 0xf2,0x38,0x87,0x45,0x98,0x0e,0xf7,0x5d,0x4e,0x6d,0xb5,0x30,0xca,0x35,0xa9,0x9b,0xf7,0xd6,0xd3,0x04,0xa2 };
// S_RUNTIME
static const unsigned char kE13[15] = { 0xff,0x3a,0x84,0x48,0xb0,0x0f,0xf4,0x7d,0x5d,0x60,0x9f,0x18,0xc2,0x33,0x94 };
// S_TAMPER
static const unsigned char kE14[36] = { 0xf0,0x2d,0x9f,0x45,0xab,0x0c,0xf7,0x5d,0x59,0x61,0xaa,0x07,0xca,0x20,0xaf,0x98,0xeb,0xc4,0xd3,0x04,0xb4,0x5e,0x4a,0x40,0x05,0x57,0x42,0x52,0x89,0x15,0xae,0xcf,0xdd,0xb0,0xfa,0xc5 };

struct Ent { const unsigned char* e; unsigned n; unsigned crc; };
static const Ent kTab[ID_COUNT] = {
    { kE0 , 10 , 0xCF52F8D2u },   // S_TITLE
    { kE1 , 22 , 0x8EC78E4Au },   // S_LOGIN_LABEL
    { kE2 , 6  , 0x57618E70u },   // S_YES
    { kE3 , 5  , 0xED1C6592u },   // S_NO
    { kE4 , 13 , 0x7E643509u },   // S_ABOUT
    { kE5 , 6  , 0x759FB403u },   // S_TAB0
    { kE6 , 6  , 0x18A16DCBu },   // S_TAB1
    { kE7 , 6  , 0x3B4B656Du },   // S_TAB2
    { kE8 , 12 , 0x5DBA8573u },   // S_TAB3
    { kE9 , 6  , 0x2C69AB15u },   // S_EXIT
    { kE10, 9  , 0x51D30981u },   // S_ANTIMUTE
    { kE11, 21 , 0x58A37CD8u },   // S_DRIVER
    { kE12, 21 , 0xF78F5CAEu },   // S_LEVEL
    { kE13, 15 , 0xE94944A3u },   // S_RUNTIME
    { kE14, 36 , 0x0B9D80B8u },   // S_TAMPER
};

// ---- 运行时：解密 + 校验 + 缓存 ----
inline bool g_tampered = false;   // 任一串校验失败 -> 置位
inline const char* Ptr[ID_COUNT] = {};
inline char Buf[ID_COUNT][128] = {};
inline bool Inited = false;

inline unsigned Crc32(const unsigned char* p, unsigned n)
{
    unsigned c = 0xFFFFFFFFu;
    for (unsigned i = 0; i < n; ++i) { c ^= p[i]; for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

// 解密+校验一条（不缓存），返回是否完好
inline bool DecodeOne(int id, char* out, unsigned cap)
{
    const Ent& t = kTab[id];
    if (t.n + 1 > cap) return false;
    for (unsigned i = 0; i < t.n; ++i) out[i] = (char)(t.e[i] ^ Key(i));
    out[t.n] = 0;
    return Crc32((const unsigned char*)out, t.n) == t.crc;
}

// 全量校验：启动时调一次；tampered 置位表示程序文字被改过
inline bool VerifyAll()
{
    g_tampered = false;
    for (int i = 0; i < ID_COUNT; ++i)
    {
        char tmp[128] = {};
        if (!DecodeOne(i, tmp, sizeof(tmp))) { g_tampered = true; return false; }
        memcpy(Buf[i], tmp, sizeof(tmp));
        Ptr[i] = Buf[i];
    }
    Inited = true;
    return true;
}

// 取串（首调自动初始化）
inline const char* Get(int id)
{
    if (!Inited) VerifyAll();
    return Ptr[id] ? Ptr[id] : "?";
}

} // namespace mio_str
