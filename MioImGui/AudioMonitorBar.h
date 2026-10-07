#pragma once

// ---------------------------------------------------------------------------
// AudioMonitorBar.h —— 监听页底部音频电平条
// 视觉：160 根极细竖条，底部固定（下面不跳）、只向上长；
//       各条高度互不相同、各自慢速原地轻跳（互不同步 → 绝无横向滚动）
// 数据：MioZegoProbe 在 TT 采集回调中实测的原始 PCM 电平（增益前）
//       共享内存 Local\MioAudioLevel_v1（无数据时全部静默）
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cmath>
#include <cstdint>

#include "imgui.h"
#include "MioZegoProbeShared.h"
#include "SystemMicCapture.h"

namespace mio_audio {

inline constexpr int kBarCount = 160; // 细条根数

inline float fractf(float x)
{
    return x - floorf(x);
}

class LevelMeter
{
public:
    // 每帧调用（与当前页无关）：采样最新电平 + 慢速平滑（不抽、不闪、不滚动）
    void Update()
    {
        // 首选：Mio 直采系统麦克风（WASAPI）
        unsigned int sys_rms = 0;
        unsigned int sys_peak = 0;
        mio_sysmic::EnsureStarted();
        if (mio_sysmic::Sample(sys_rms, sys_peak))
        {
            const float v = (float)sys_peak / 1000.0f;
            smooth_ += (v - smooth_) * 0.08f; // 慢跟随：慢慢起、慢慢落
            return;
        }

        // 兜底：TT 载荷共享内存（系统直采不可用时）
        if (!view_)
        {
            if (!Open())
                return;
        }

        const uint32_t s1 = view_->seq;
        if (s1 & 1u)
            return;
        const uint32_t peak = view_->peak_milli;
        const uint64_t tick = view_->tick_ms;
        if (view_->seq != s1)
            return;

        const float v = (float)peak / 1000.0f;
        if (tick != last_tick_)
        {
            last_tick_ = tick;
            smooth_ += (v - smooth_) * 0.08f; // 慢跟随：慢慢起、慢慢落
        }
        else
        {
            smooth_ *= 0.985f; // 无新数据 → 很慢地回落
        }
    }

    // 监听页调用：底边固定、只向上长（可窜过设备文字，画在文字下层）；reserve_only=仅占位
    void Draw(float scale, bool reserve_only = false)
    {
        if (reserve_only)
        {
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 30.0f * scale)); // 沿用旧版占位
            return;
        }
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetIO().DisplaySize.y - 41.0f * scale;
        // 向上行程拉满：从基线一路加长到卡片顶部（可以窜过上方设备列表文字；只向上、底边固定）
        float h = 200.0f * scale;
        {
            const float win_top = ImGui::GetWindowPos().y + 60.0f * scale; // 卡在标题/tab 行之下，不越界
            const float h_cap = (bottom - 2.0f * scale) - win_top;
            if (h > h_cap)
                h = h_cap;
        }
        if (w < 24.0f || h < 10.0f)
            return;
        const float top = bottom - h;
        const ImVec2 p0(cur.x, top);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 底槽已移除（调试粉白来源；确认后再决定是否恢复）

        float v = smooth_;
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;

        const float pitch = w / (float)kBarCount;
        const float barw = pitch * 0.62f; // 非常细
        const float x_off = (pitch - barw) * 0.5f;
        const float y_base = bottom - 2.0f * scale; // 固定基线：下面不动
        const float max_h = h - 5.0f * scale;
        const float t = (float)ImGui::GetTime();

        for (int i = 0; i < kBarCount; ++i)
        {
            // 每根条自己的固定形状 + 慢速原地轻摆；各条互不同步 → 不是横向流动
            const float r1 = fractf(sinf((float)i * 12.9898f) * 43758.5453f);
            const float r2 = fractf(sinf((float)i * 4.1237f + 2.71f) * 24634.6345f);
            float m = 0.42f + 0.58f * r1 + 0.16f * sinf(t * (0.55f + 0.85f * r2) + r1 * 6.2831f);
            if (m < 0.25f)
                m = 0.25f;
            if (m > 1.0f)
                m = 1.0f;

            float bh = v * max_h * m;
            if (bh < 1.0f * scale)
                bh = 1.0f * scale; // 静默时底部留小点

            const float x = p0.x + pitch * (float)i + x_off;

            // 渐变：linear-gradient(135deg, #E8D5FF 0%, #A4C7FF 100%)
            // 条顶=淡紫、条底=淡蓝；135° 斜向感：左亮右深的横向递进
            const ImVec4 c_light(0xE8 / 255.0f, 0xD5 / 255.0f, 0xFF / 255.0f, 1.0f); // #E8D5FF
            const ImVec4 c_dark(0xA4 / 255.0f, 0xC7 / 255.0f, 0xFF / 255.0f, 1.0f);  // #A4C7FF
            const float xf = (float)i / (float)(kBarCount - 1); // 0=左, 1=右
            const float dtop = 0.10f + 0.25f * xf;             // 顶色斜向加深
            const float dbot = 0.80f + 0.20f * xf;             // 底色斜向加深
            const ImVec4 c_top(c_light.x + (c_dark.x - c_light.x) * dtop,
                               c_light.y + (c_dark.y - c_light.y) * dtop,
                               c_light.z + (c_dark.z - c_light.z) * dtop, 1.0f);
            const ImVec4 c_bot(c_light.x + (c_dark.x - c_light.x) * dbot,
                               c_light.y + (c_dark.y - c_light.y) * dbot,
                               c_light.z + (c_dark.z - c_light.z) * dbot, 1.0f);
            const ImU32 top_col = ImGui::ColorConvertFloat4ToU32(c_top);
            const ImU32 bot_col = ImGui::ColorConvertFloat4ToU32(c_bot);
            dl->AddRectFilledMultiColor(ImVec2(x, y_base - bh), ImVec2(x + barw, y_base),
                                        top_col, top_col, bot_col, bot_col);
        }

    }

private:
    bool Open()
    {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, MIO_LEVEL_SHARED_NAME);
        if (!mapping)
            return false;
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(MioAudioLevelShared));
        if (!view)
        {
            CloseHandle(mapping);
            return false;
        }
        mapping_ = mapping;
        view_ = (const MioAudioLevelShared*)view;
        if (view_->magic != MIO_LEVEL_SHARED_MAGIC)
        {
            Close();
            view_ = nullptr;
            return false;
        }
        return true;
    }

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

    HANDLE mapping_ = nullptr;
    const MioAudioLevelShared* view_ = nullptr;
    float smooth_ = 0.0f;
    uint64_t last_tick_ = 0;
};

inline LevelMeter& Level()
{
    static LevelMeter meter;
    return meter;
}

} // namespace mio_audio
