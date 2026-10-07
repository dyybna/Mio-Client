#pragma once
// ---------------------------------------------------------------------------
// MioIntegrity.h —— 防篡改（自身完整性校验）
//
// 原理：
//   1. 编译期在 .rdata 里放一个 64 字节"定位块"：
//        [0..32)  = 32 字节魔数 "MIO-TAMPER-CHECK" + 16 字节二进制特征
//        [32..64) = 32 字节 SHA-256 槽（初始全 0）
//   2. 构建后脚本 tools\pack_exe.ps1 会：
//        - 用同样的算法算出"PE 映像部分"的 SHA-256（跳过这 64 字节槽）
//        - 把结果写进槽里
//        - 再把文件填充到 40MB（填充在映像之外，不参与校验）
//   3. 运行时读自身文件、复算、比对；不一致 = 被改过。
//
// 说明：跳过的是"槽本身"，所以写哈希不会影响哈希 —— 这是自校验的经典做法。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mio_integrity {

// ---- 定位块（必须与 tools/pack_exe.ps1 里的常量完全一致）----
static const unsigned char kMarker[64] = {
    // 32 字节魔数
    'M','I','O','-','T','A','M','P','E','R','-','C','H','E','C','K',
    0x9E,0x3B,0x7F,0x21,0xC4,0x58,0xA6,0x0D,0x11,0xF2,0x84,0x4B,0x6C,0xE5,0x97,0x30,
    // 32 字节 SHA-256 槽（构建后写入）
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0
};
static const size_t kMarkerSize = sizeof(kMarker);
static const size_t kHashOff    = 32;   // 哈希在定位块内的偏移
static const size_t kHashLen    = 32;

// ---------------------------------------------------------------------------
// 紧凑 SHA-256（自包含，不依赖任何加密库）
// ---------------------------------------------------------------------------
class Sha256
{
public:
    Sha256() { Reset(); }

    void Reset()
    {
        h_[0] = 0x6a09e667u; h_[1] = 0xbb67ae85u; h_[2] = 0x3c6ef372u; h_[3] = 0xa54ff53au;
        h_[4] = 0x510e527fu; h_[5] = 0x9b05688cu; h_[6] = 0x1f83d9abu; h_[7] = 0x5be0cd19u;
        total_ = 0; fill_ = 0;
    }

    void Update(const void* data, size_t len)
    {
        const unsigned char* p = (const unsigned char*)data;
        total_ += (uint64_t)len;
        while (len)
        {
            size_t take = 64 - fill_;
            if (take > len) take = len;
            memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; len -= take;
            if (fill_ == 64) { Block(buf_); fill_ = 0; }
        }
    }

    void Final(unsigned char out[32])
    {
        const uint64_t bits = total_ * 8;         // 注意：先记下来，再补位
        unsigned char one = 0x80, zero = 0x00;
        Update(&one, 1);
        while (fill_ != 56) Update(&zero, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; ++i) lb[7 - i] = (unsigned char)(bits >> (8 * i));
        Update(lb, 8);
        for (int i = 0; i < 8; ++i)
        {
            out[i * 4 + 0] = (unsigned char)(h_[i] >> 24);
            out[i * 4 + 1] = (unsigned char)(h_[i] >> 16);
            out[i * 4 + 2] = (unsigned char)(h_[i] >> 8);
            out[i * 4 + 3] = (unsigned char)(h_[i]);
        }
    }

private:
    static uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Block(const unsigned char* p)
    {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Ror(w[i-15],7) ^ Ror(w[i-15],18) ^ (w[i-15] >> 3);
            const uint32_t s1 = Ror(w[i-2],17) ^ Ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t S1 = Ror(e,6) ^ Ror(e,11) ^ Ror(e,25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Ror(a,2) ^ Ror(a,13) ^ Ror(a,22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
    }

    uint32_t h_[8];
    uint64_t total_;
    size_t   fill_;
    unsigned char buf_[64];
};

// ---------------------------------------------------------------------------
// 结果状态
// ---------------------------------------------------------------------------
enum class Status { Unknown, Pass, Fail, NoMarker, Error, Unpacked };

inline Status&       St()      { static Status s = Status::Unknown; return s; }
inline std::string&  Detail()  { static std::string d; return d; }
inline unsigned char* HashOut() { static unsigned char h[32] = {}; return h; }

inline std::string Hex32(const unsigned char* h, int n = 32)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; ++i) { s += k[h[i] >> 4]; s += k[h[i] & 15]; }
    return s;
}

inline void ToLog(const char* fmt, ...)
{
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    std::wstring dir = base; dir += L"\\Mio";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\integrity.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// PE 映像占用到哪个文件偏移（最后一个节区的 数据指针+大小）
inline bool PeEnd(size_t& end)
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;

    unsigned char hdr[0x400];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'M' || hdr[1] != 'Z')
    { fclose(f); return false; }
    const uint32_t e_lfanew = *(uint32_t*)(hdr + 0x3C);
    if (*(uint32_t*)(hdr + e_lfanew) != 0x00004550u) { fclose(f); return false; }
    const uint16_t nsec = *(uint16_t*)(hdr + e_lfanew + 6);
    const uint16_t optsz = *(uint16_t*)(hdr + e_lfanew + 20);
    const size_t secoff = e_lfanew + 24 + optsz;
    const size_t need = secoff + (size_t)nsec * 40;   // 节表可能远大于 0x400（本 exe 有 18 个节）
    if (need < sizeof(hdr) || need > (64u << 10)) { fclose(f); return false; }

    // 重新按需读取完整节表（之前只读 0x400 会把靠后的节区当成不存在 -> 哈希范围偏小）
    static unsigned char s_tbl[64u << 10];
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    if (fread(s_tbl, 1, need, f) != need) { fclose(f); return false; }
    fclose(f);

    size_t maxend = need;
    for (int i = 0; i < nsec; ++i)
    {
        const unsigned char* s = s_tbl + secoff + (size_t)i * 40;
        const uint32_t rawsz = *(uint32_t*)(s + 16);
        const uint32_t rawptr = *(uint32_t*)(s + 20);
        const size_t e = (size_t)rawptr + rawsz;
        if (e > maxend) maxend = e;
    }
    end = maxend;
    return true;
}

// 读取自身文件并校验（跳过定位块自身的 64 字节）
inline bool Verify()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) { St() = Status::Error; Detail() = "取自身路径失败"; return false; }

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { St() = Status::Error; Detail() = "打不开自身文件"; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t end = 0;
    if (!PeEnd(end)) { CloseHandle(h); St() = Status::Error; Detail() = "解析 PE 失败"; return false; }
    if ((unsigned long long)end > (unsigned long long)sz.QuadPart) end = (size_t)sz.QuadPart;

    std::string data; data.resize(end);
    DWORD got = 0;
    if (!ReadFile(h, &data[0], (DWORD)end, &got, nullptr) || got != end)
    { CloseHandle(h); St() = Status::Error; Detail() = "读取自身失败"; return false; }
    CloseHandle(h);

    // 在文件里定位 32 字节魔数（用 ASCII 段先粗定位，再核对整段）
    static const char kAscii[17] = "MIO-TAMPER-CHECK";
    size_t off = std::string::npos;
    for (size_t i = 0; i + kMarkerSize <= data.size(); ++i)
    {
        if (memcmp(data.data() + i, kAscii, 16) == 0 && memcmp(data.data() + i, kMarker, 32) == 0)
        { off = i; break; }
    }
    if (off == std::string::npos)
    {
        St() = Status::NoMarker;
        Detail() = "未找到防篡改标记（可能是未经过 pack_exe.ps1 的未打包版本）";
        ToLog("integrity: 未找到标记 -> %s", Detail().c_str());
        return false;
    }

    Sha256 sha;
    sha.Update(data.data(), off);
    sha.Update(data.data() + off + kMarkerSize, data.size() - off - kMarkerSize);
    unsigned char dg[32];
    sha.Final(dg);
    memcpy(HashOut(), dg, 32);

    const unsigned char* slot = (const unsigned char*)data.data() + off + kHashOff;
    bool allZero = true;
    for (size_t i = 0; i < kHashLen; ++i) if (slot[i]) { allZero = false; break; }

    if (allZero)
    {
        St() = Status::Unpacked;
        Detail() = "哈希槽为空（没跑过打包脚本）";
        ToLog("integrity: %s  hash=%s", Detail().c_str(), Hex32(dg).c_str());
        return false;
    }
    if (memcmp(slot, dg, 32) == 0)
    {
        St() = Status::Pass;
        Detail() = "通过";
        ToLog("integrity: 通过  hash=%s", Hex32(dg).c_str());
        return true;
    }
    St() = Status::Fail;
    Detail() = "不一致：文件已被修改";
    ToLog("integrity: 校验失败  期望=%s 实际=%s", Hex32(slot).c_str(), Hex32(dg).c_str());
    return false;
}

inline bool Genuine() { return St() == Status::Pass; }

inline const char* StatusText()
{
    switch (St())
    {
    case Status::Pass:     return "通过";
    case Status::Fail:     return "失败";
    case Status::NoMarker: return "无标记";
    case Status::Unpacked: return "未打包";
    case Status::Error:    return "异常";
    default:               return "未校验";
    }
}

} // namespace mio_integrity
