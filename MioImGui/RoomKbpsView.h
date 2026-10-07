#pragma once

// ---------------------------------------------------------------------------
// RoomKbpsView.h —— 「房间码率」页
// 数据来自 MioZegoProbe.dll 注入 TT 后写入的共享内存：
//   每个房间成员的拉流 audioKBPS + 自己的推流 audioKBPS。
// 注入全自动（无按钮无开关）；页面只显示「名字 + kbps」两列。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "ZegoProbeClient.h"

namespace mio_roomkbps {

struct RowData
{
    std::string name;
    double kbps = 0.0;
    bool self = false;
    bool stale = false;
};

// ---------------------------------------------------------------------------

class View
{
public:
    // 侧信息目标码率（Mio 同步自 Mio.ini 的 Kbps=；房间页「我」行按动态补差后的目标对齐显示）
    void SetBoostTarget(int kbps) { boost_target_ = kbps; }

    // 每帧调用（与当前页无关）：扫描目标 / 自动注入 / 读取共享内存
    void Tick()
    {
        const double now = ImGui::GetTime();
        // 未接上数据时高频扫描（0.5s），抢在即构注册之前注入；连上后降频省资源
        const double scan_interval = reader_.Valid() ? 2.0 : 0.5;
        if (last_scan_ < 0.0 || now - last_scan_ >= scan_interval)
        {
            Scan(now);
            last_scan_ = now;
        }

        if (reader_.Valid())
        {
            if (reader_.IsLegacy())
            {
                legacy_ok_ = reader_.ReadV1(v1_snap_);
                snap_ok_ = false;
            }
            else
            {
                snap_ok_ = reader_.Read(snap_);
                legacy_ok_ = false;
            }
        }
        else
        {
            snap_ok_ = false;
            legacy_ok_ = false;
            if (now - last_open_try_ >= 0.5)
            {
                last_open_try_ = now;
                reader_.Open(); // 载荷启动后共享内存才存在，定时重试
            }
        }
    }

    // 仅在「房间码率」页可见时调用：只画名字 + kbps 列表
    void Draw(float scale, bool tt_active, ImFont* font, ImFont* font_bold)
    {
        (void)tt_active;
        (void)font_bold;
        DrawList(scale, font, ImGui::GetTime());
    }

private:
    struct FailRetry
    {
        DWORD pid = 0;
        double retry_at = 0.0;
    };

    static int TargetScore(const mio_zego::TargetProcess& t)
    {
        return (t.hasProbe ? 8 : 0) + (t.hasZego ? 4 : 0) +
               (t.hasNodeAddon ? 2 : 0) + (t.isRoomRenderer ? 1 : 0);
    }

    static void SortRows(std::vector<RowData>& rows)
    {
        std::stable_sort(rows.begin(), rows.end(), [](const RowData& a, const RowData& b) {
            if (a.self != b.self)
                return a.self;
            if (a.stale != b.stale)
                return !a.stale;
            return a.kbps > b.kbps;
        });
    }

    // ---- 扫描 / 注入 ------------------------------------------------------

    void Scan(double now)
    {
        targets_.clear();
        mio_zego::CollectTargets(targets_);

        // 展示目标：优先已注入 > Zego/Node > 语音 renderer
        target_found_ = false;
        int best_score = -1;
        for (const mio_zego::TargetProcess& t : targets_)
        {
            if (!t.injectable && !t.hasProbe)
                continue;
            const int score = TargetScore(t);
            if (score > best_score)
            {
                best_score = score;
                target_ = t;
                target_found_ = true;
            }
        }
        probe_present_ = target_found_ && target_.hasProbe;

        // 清理已消失进程的记录
        for (size_t i = 0; i < injected_ok_.size();)
        {
            bool alive = false;
            for (const auto& t : targets_)
                if (t.pid == injected_ok_[i]) { alive = true; break; }
            if (alive) ++i; else injected_ok_.erase(injected_ok_.begin() + i);
        }
        for (size_t i = 0; i < fail_retry_.size();)
        {
            bool alive = false;
            for (const auto& t : targets_)
                if (t.pid == fail_retry_[i].pid) { alive = true; break; }
            if (alive) ++i; else fail_retry_.erase(fail_retry_.begin() + i);
        }

        // 全自动注入：语音 renderer 一出生就抢注入（配合载荷预载 = 注册零竞态）
        {
            for (const mio_zego::TargetProcess& t : targets_)
            {
                if (!t.injectable || t.hasProbe)
                    continue;
                bool done = false;
                for (DWORD pid : injected_ok_)
                    if (pid == t.pid) { done = true; break; }
                if (done)
                    continue;
                double retry_at = 0.0;
                for (const FailRetry& f : fail_retry_)
                    if (f.pid == t.pid) retry_at = f.retry_at;
                if (now < retry_at)
                    continue;
                if (now - last_inject_try_ < 0.5)
                    break;
                last_inject_try_ = now;
                DoInject(t, now);
                break; // 一轮注入一个目标
            }
        }
    }

    void DoInject(const mio_zego::TargetProcess& t, double now)
    {
        if (t.hasProbe)
            return;

        // 从内嵌载荷释放出的路径注入（优先 C:\Windows，退回用户目录）
        const std::wstring dll = mio_zego::ExtractBundledProbe();
        if (dll.empty())
            return;

        std::wstring err;
        if (mio_zego::InjectProbe(t.pid, dll, err))
        {
            injected_ok_.push_back(t.pid);
            reader_.Open();
        }
        else
        {
            bool found = false;
            for (FailRetry& f : fail_retry_)
                if (f.pid == t.pid) { f.retry_at = now + 20.0; found = true; }
            if (!found)
            {
                FailRetry fr;
                fr.pid = t.pid;
                fr.retry_at = now + 20.0;
                fail_retry_.push_back(fr);
            }
        }
    }

    void DrawList(float scale, ImFont* font, double now)
    {
        (void)now;
        std::vector<RowData> rows;

        if (snap_ok_)
        {
            const uint64_t now_ms = GetTickCount64();

            // 自己：取最近一条推流条目
            const MioZegoStreamEntry* best_pub = nullptr;
            for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
            {
                const MioZegoStreamEntry& e = snap_.entries[i];
                if (e.direction != MIO_ZEGO_DIR_PUBLISH || !e.stream_id[0])
                    continue;
                if (!best_pub || e.last_seen_ms > best_pub->last_seen_ms)
                    best_pub = &e;
            }
            if (best_pub && now_ms > best_pub->last_seen_ms &&
                now_ms - best_pub->last_seen_ms > MIO_ZEGO_STALE_MS)
                best_pub = nullptr; // 已停播超时 → 不显示自己那行
            if (best_pub)
            {
                RowData r;
                r.name = "我";
                r.kbps = best_pub->audio_kbps;
                // 载荷按【目标−底流】动态补差（静音段补差、满码率停补），合计恒 ≈ 目标值。
                // 已同步到有效目标（≥200）时直接对齐显示；无配置（0）时回落真实底流。
                if (boost_target_ >= 200)
                    r.kbps = (double)boost_target_;
                r.self = true;
                r.stale = (now_ms > best_pub->last_seen_ms) &&
                          (now_ms - best_pub->last_seen_ms > 6000);
                rows.push_back(r);
            }

            // 房间成员：只显示仍活跃的拉流条目（离开/停推 12 秒后自动消失）
            for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
            {
                const MioZegoStreamEntry& e = snap_.entries[i];
                if (e.direction != MIO_ZEGO_DIR_PLAY || !e.stream_id[0])
                    continue;
                if (now_ms > e.last_seen_ms && now_ms - e.last_seen_ms > MIO_ZEGO_STALE_MS)
                    continue; // 超时未更新 → 视为已离开，不再显示
                RowData r;
                r.name = e.user_name[0] ? std::string(e.user_name) : std::string("…");
                r.kbps = e.audio_kbps;
                r.stale = (now_ms > e.last_seen_ms) && (now_ms - e.last_seen_ms > 6000);
                rows.push_back(r);
            }

            SortRows(rows);
        }
        else if (legacy_ok_)
        {
            // 旧版载荷：可以读码率，但没有昵称字段
            const uint64_t now_ms = GetTickCount64();

            const mio_zego::MioZegoV1Entry* best_pub = nullptr;
            for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
            {
                const mio_zego::MioZegoV1Entry& e = v1_snap_.entries[i];
                if (e.direction != MIO_ZEGO_DIR_PUBLISH || !e.stream_id[0])
                    continue;
                if (!best_pub || e.last_seen_ms > best_pub->last_seen_ms)
                    best_pub = &e;
            }
            if (best_pub && now_ms > best_pub->last_seen_ms &&
                now_ms - best_pub->last_seen_ms > MIO_ZEGO_STALE_MS)
                best_pub = nullptr; // 已停播超时 → 不显示自己那行
            if (best_pub)
            {
                RowData r;
                r.name = "我";
                r.kbps = best_pub->audio_kbps;
                r.self = true;
                r.stale = (now_ms > best_pub->last_seen_ms) &&
                          (now_ms - best_pub->last_seen_ms > 6000);
                rows.push_back(r);
            }

            for (uint32_t i = 0; i < MIO_ZEGO_MAX_STREAMS; ++i)
            {
                const mio_zego::MioZegoV1Entry& e = v1_snap_.entries[i];
                if (e.direction != MIO_ZEGO_DIR_PLAY || !e.stream_id[0])
                    continue;
                if (now_ms > e.last_seen_ms && now_ms - e.last_seen_ms > MIO_ZEGO_STALE_MS)
                    continue; // 超时未更新 → 视为已离开，不再显示
                RowData r;
                r.name = "…";
                r.kbps = e.audio_kbps;
                r.stale = (now_ms > e.last_seen_ms) && (now_ms - e.last_seen_ms > 6000);
                rows.push_back(r);
            }

            SortRows(rows);
        }

        ImGui::PushFont(font, 15.0f);
        ImGui::BeginChild("##MioRoomKbpsList", ImVec2(0.0f, 108.0f * scale), false);
            for (const RowData& r : rows)
        {
                char kb[32] = {};
                if (r.kbps >= 10.0)
                    snprintf(kb, sizeof(kb), "%.0f", r.kbps);
                else
                    snprintf(kb, sizeof(kb), "%.1f", r.kbps);

                char line[256] = {};
                snprintf(line, sizeof(line), "%s   %s kbps", r.name.c_str(), kb);

                ImVec4 color;
                if (r.stale)
                    color = ImVec4(0.62f, 0.62f, 0.66f, 1.0f);
                else if (r.kbps >= 48.0)
                    color = ImVec4(0.13f, 0.62f, 0.35f, 1.0f);
                else if (r.kbps >= 24.0)
                    color = ImVec4(0.80f, 0.52f, 0.06f, 1.0f);
                else
                    color = ImVec4(0.80f, 0.24f, 0.30f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(line);
                ImGui::PopStyleColor();
            }
        ImGui::EndChild();
        ImGui::PopFont();
    }

    // ---- 状态 --------------------------------------------------------------

    int boost_target_ = 0; // 侧信息目标（SetBoostTarget 同步；≥200 且侧信息激活时「我」行直接显示该值）
    double last_scan_ = -1.0;
    double last_open_try_ = -100.0;
    double last_inject_try_ = -100.0;

    bool target_found_ = false;
    bool probe_present_ = false;
    mio_zego::TargetProcess target_;
    std::vector<mio_zego::TargetProcess> targets_;
    std::vector<DWORD> injected_ok_;
    std::vector<FailRetry> fail_retry_;

    mio_zego::SharedReader reader_;
    MioZegoShared snap_ = {};
    bool snap_ok_ = false;
    mio_zego::MioZegoV1Shared v1_snap_ = {};
    bool legacy_ok_ = false;
};

} // namespace mio_roomkbps
