

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include "RoomKbpsView.h"
#include "AudioMonitorBar.h"
#include "SystemMicCapture.h"
#include "MioGain.h"
#include "MioIntegrity.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static HGLRC g_hRC = nullptr;
static HDC   g_hDC = nullptr;
static int   g_Width = 0;
static int   g_Height = 0;

static ImFont* g_font = nullptr;
static ImFont* g_font_bold = nullptr;
static float   g_scale = 1.0f;

static bool        g_should_close = false;
static bool        g_anti_mute = false;
static bool        g_anti_mute_sent = false;
static int         g_active_tab = 0;
static mio_roomkbps::View g_room_kbps;
static ImRect      g_drag_exclude[16];
static int         g_drag_exclude_count = 0;
static bool        g_imgui_ready = false;
static double      g_last_device_refresh = -1.0;
static std::vector<std::wstring> g_active_input;
static std::vector<std::wstring> g_active_output;
static bool   g_tt_active = false;
static double g_last_tt_check = -1.0;

static bool g_login_open = true;
static char g_login_code[128] = "";
static bool g_login_focus_once = true;

static ImU32 RainbowAt(float t)
{
    static const ImVec4 palette[5] = {
        ImVec4(0.600f, 0.200f, 0.867f, 1.0f),
        ImVec4(0.267f, 0.467f, 1.000f, 1.0f),
        ImVec4(0.200f, 0.867f, 0.733f, 1.0f),
        ImVec4(0.933f, 0.867f, 0.200f, 1.0f),
        ImVec4(1.000f, 0.267f, 0.533f, 1.0f),
    };

    t = fmodf(t, 5.0f);
    if (t < 0.0f)
        t += 5.0f;

    const int i = (int)t;
    const float f = t - (float)i;
    const ImVec4& a = palette[i];
    const ImVec4& b = palette[(i + 1) % 5];

    ImVec4 c;
    c.x = a.x + (b.x - a.x) * f;
    c.y = a.y + (b.y - a.y) * f;
    c.z = a.z + (b.z - a.z) * f;
    c.w = 1.0f;
    return ImGui::ColorConvertFloat4ToU32(c);
}
static ImVec4 g_grad_stops[8];
static int    g_grad_stop_count = 0;

static ImU32 Gradient4(float t)
{
    static const ImVec4 def[4] = {
        ImVec4(0.600f, 0.200f, 0.867f, 1.0f),
        ImVec4(0.267f, 0.467f, 1.000f, 1.0f),
        ImVec4(0.200f, 0.867f, 0.733f, 1.0f),
        ImVec4(0.933f, 0.867f, 0.200f, 1.0f),
    };
    const ImVec4* stops = (g_grad_stop_count >= 2) ? g_grad_stops : def;
    const int n = (g_grad_stop_count >= 2) ? g_grad_stop_count : 4;
    t = ImClamp(t, 0.0f, 1.0f) * (float)(n - 1);
    int i = (int)t;
    if (i >= n - 1)
        i = n - 2;
    if (i < 0)
        i = 0;
    const float f = t - (float)i;
    const ImVec4& a = stops[i];
    const ImVec4& b = stops[i + 1];
    ImVec4 c;
    c.x = a.x + (b.x - a.x) * f;
    c.y = a.y + (b.y - a.y) * f;
    c.z = a.z + (b.z - a.z) * f;
    c.w = 1.0f;
    return ImGui::ColorConvertFloat4ToU32(c);
}

static bool IsTTVoiceEntertainmentRunning()
{
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"TT语音娱乐版.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"TT语音娱乐版") == 0)
            {
                found = true;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
}

static HWND g_status_overlay = nullptr;
static bool g_status_overlay_state = false;
static bool g_status_overlay_has_state = false;

static LRESULT CALLBACK StatusOverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_DESTROY)
        return 0;
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void UpdateStatusOverlay(HWND hWnd, bool active)
{
    if (!hWnd)
        return;

    constexpr int kWidth = 320;
    constexpr int kHeight = 96;
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        if (bitmap) ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old_bitmap = ::SelectObject(memory, bitmap);
    auto* pixels = static_cast<unsigned char*>(bits);
    ::ZeroMemory(pixels, (size_t)kWidth * kHeight * 4);

    HFONT font = ::CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HFONT small_font = ::CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    ::SetBkMode(memory, TRANSPARENT);
    static ULONGLONG overlay_start_tick = 0;
    if (!overlay_start_tick)
        overlay_start_tick = ::GetTickCount64();
    const float phase = (float)((double)(::GetTickCount64() - overlay_start_tick) * 0.0001);
    auto draw_gradient_text = [&](HFONT text_font, const wchar_t* text, int y, int start_r, int start_g, int start_b,
                                  int end_r, int end_g, int end_b)
    {
        ::SelectObject(memory, text_font);
        SIZE total{};
        const int length = lstrlenW(text);
        ::GetTextExtentPoint32W(memory, text, length, &total);
        int x = (kWidth - total.cx) / 2;
        for (int i = 0; i < length; ++i)
        {
            SIZE glyph{};
            ::GetTextExtentPoint32W(memory, text + i, 1, &glyph);
            float u = (length > 1 ? (float)i / (float)(length - 1) : 0.0f) * 0.5f + phase;
            u -= (float)(int)u;
            const float t = (u < 0.5f) ? (u * 2.0f) : (2.0f - u * 2.0f);
            const int r = (int)(start_r + (end_r - start_r) * t);
            const int g = (int)(start_g + (end_g - start_g) * t);
            const int b = (int)(start_b + (end_b - start_b) * t);
            ::SetTextColor(memory, RGB(r, g, b));
            ::TextOutW(memory, x, y, text + i, 1);
            x += glyph.cx;
        }
    };
    if (active)
    {
        draw_gradient_text(font, L"操你妈71", 9, 153, 51, 221, 51, 221, 187);
    }
    else
    {
        draw_gradient_text(font, L"MioDebug", 9, 138, 127, 184, 95, 158, 147);
    }
    for (int y = 0; y < kHeight; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            const size_t offset = ((size_t)y * kWidth + x) * 4;
            const unsigned char r8 = pixels[offset + 0];
            const unsigned char g8 = pixels[offset + 1];
            const unsigned char b8 = pixels[offset + 2];
            const unsigned char m = (r8 > g8) ? (r8 > b8 ? r8 : b8) : (g8 > b8 ? g8 : b8);
            pixels[offset + 3] = m;
        }
    }
    if (font) ::DeleteObject(font);
    if (small_font) ::DeleteObject(small_font);

    RECT work_area{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    POINT destination{ work_area.right - kWidth - 16, work_area.top + 16 };
    SIZE size{ kWidth, kHeight };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ::UpdateLayeredWindow(hWnd, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);
    ::SelectObject(memory, old_bitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    ::ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hWnd, HWND_TOPMOST, destination.x, destination.y, kWidth, kHeight,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static const char* Utf8Next(const char* p)
{
    const unsigned char c = (unsigned char)*p;
    int n = 1;
    if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;
    for (int i = 1; i < n && *p; ++i)
        ++p;
    return p + 1;
}

static float TextWidth(ImFont* font, float size, const char* text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

static void DrawRainbowText(ImDrawList* dl, ImFont* font, float font_size, const ImVec2& pos, const char* text, float time)
{
    const float total_w = TextWidth(font, font_size, text);
    float x = pos.x;
    const char* p = text;
    while (*p)
    {
        const char* begin = p;
        const char* end = Utf8Next(p);
        const ImVec2 sz = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end);
        const float u = total_w > 0.0f ? (x - pos.x) / total_w : 0.0f;
        const ImU32 col = RainbowAt(u * 1.3f + time * 0.3f);
        dl->AddText(font, font_size, ImVec2(x, pos.y), col, begin, end);
        x += sz.x;
        p = end;
    }
}

static void DrawGradientText(ImDrawList* dl, ImFont* font, float font_size, const ImVec2& pos, const char* text, const ImVec4& col_start, const ImVec4& col_end)
{
    const float total_w = TextWidth(font, font_size, text);
    float x = pos.x;
    const char* p = text;
    while (*p)
    {
        const char* begin = p;
        const char* end = Utf8Next(p);
        const ImVec2 sz = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end);
        const float u = total_w > 0.0f ? (x - pos.x) / total_w : 0.0f;
        ImVec4 c;
        c.x = col_start.x + (col_end.x - col_start.x) * u;
        c.y = col_start.y + (col_end.y - col_start.y) * u;
        c.z = col_start.z + (col_end.z - col_start.z) * u;
        c.w = 1.0f;
        dl->AddText(font, font_size, ImVec2(x, pos.y), ImGui::ColorConvertFloat4ToU32(c), begin, end);
        x += sz.x;
        p = end;
    }
}

static void RowWithRainbow(const char* prefix, const char* value, float time)
{
    const float font_size = 20.0f * g_scale;
    const float line_h = 24.0f * g_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float ty = pos.y + (line_h - font_size) * 0.5f;
    const float prefix_w = TextWidth(g_font, font_size, prefix);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(g_font, font_size, ImVec2(pos.x, ty), IM_COL32(40, 40, 60, 255), prefix);
    DrawRainbowText(dl, g_font_bold, font_size, ImVec2(pos.x + prefix_w, ty), value, time);
    ImGui::Dummy(ImVec2(prefix_w + TextWidth(g_font_bold, font_size, value), line_h));
}

static void RowWithGradient(const char* prefix, const char* value)
{
    const float font_size = 20.0f * g_scale;
    const float line_h = 24.0f * g_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float ty = pos.y + (line_h - font_size) * 0.5f;
    const float prefix_w = TextWidth(g_font, font_size, prefix);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(g_font, font_size, ImVec2(pos.x, ty), IM_COL32(40, 40, 60, 255), prefix);
    DrawGradientText(dl, g_font_bold, font_size, ImVec2(pos.x + prefix_w, ty), value,
                     ImVec4(0.20f, 0.10f, 0.80f, 1.0f), ImVec4(0.90f, 0.20f, 0.90f, 1.0f));
    ImGui::Dummy(ImVec2(prefix_w + TextWidth(g_font_bold, font_size, value), line_h));
}

static void RowWithGradientColors(const char* prefix, const char* value, const ImVec4& col_start, const ImVec4& col_end)
{
    const float font_size = 20.0f * g_scale;
    const float line_h = 24.0f * g_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float ty = pos.y + (line_h - font_size) * 0.5f;
    const float prefix_w = TextWidth(g_font, font_size, prefix);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(g_font, font_size, ImVec2(pos.x, ty), IM_COL32(40, 40, 60, 255), prefix);
    DrawGradientText(dl, g_font_bold, font_size, ImVec2(pos.x + prefix_w, ty), value, col_start, col_end);
    ImGui::Dummy(ImVec2(prefix_w + TextWidth(g_font_bold, font_size, value), line_h));
}

static void RowWithSolid(const char* prefix, const char* value, const ImVec4& col)
{
    const float font_size = 20.0f * g_scale;
    const float line_h = 24.0f * g_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float ty = pos.y + (line_h - font_size) * 0.5f;
    const float prefix_w = TextWidth(g_font, font_size, prefix);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(g_font, font_size, ImVec2(pos.x, ty), IM_COL32(40, 40, 60, 255), prefix);
    dl->AddText(g_font_bold, font_size, ImVec2(pos.x + prefix_w, ty), ImGui::ColorConvertFloat4ToU32(col), value);
    ImGui::Dummy(ImVec2(prefix_w + TextWidth(g_font_bold, font_size, value), line_h));
}

static void TextWideIndented(const WCHAR* text)
{
    char buf[512];
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf), nullptr, nullptr);
    if (len <= 0)
        return;
    std::string line = "  ";
    line += buf;
    ImGui::TextUnformatted(line.c_str());
}

static std::wstring GetDeviceFriendlyName(IMMDevice* device)
{
    std::wstring name = L"设备";
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)))
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
            name = var.pwszVal;
        PropVariantClear(&var);
        store->Release();
    }
    return name;
}

static void CollectActiveDevices(bool input, std::vector<std::wstring>& out)
{
    out.clear();
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
    {
        ::CoUninitialize();
        return;
    }

    IMMDeviceCollection* collection = nullptr;
    const EDataFlow flow = input ? eCapture : eRender;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) && collection)
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device)
                continue;

            bool active_session = false;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)))
            {
                IAudioSessionEnumerator* sessions = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions)))
                {
                    int scount = 0;
                    sessions->GetCount(&scount);
                    for (int s = 0; s < scount; ++s)
                    {
                        IAudioSessionControl* ctrl = nullptr;
                        if (SUCCEEDED(sessions->GetSession(s, &ctrl)))
                        {
                            AudioSessionState state = AudioSessionStateInactive;
                            if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive)
                                active_session = true;
                            ctrl->Release();
                        }
                        if (active_session)
                            break;
                    }
                    sessions->Release();
                }
                mgr->Release();
            }

            if (active_session)
                out.push_back(GetDeviceFriendlyName(device));
            device->Release();
        }
        collection->Release();
    }
    if (enumerator)
        enumerator->Release();
    ::CoUninitialize();
}

static bool MioButton(const char* label, const ImVec4& bg, const ImVec4& bg_hover, const ImVec4& bg_active, float font_size = 20.0f)
{
    ImGui::PushFont(g_font, font_size);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size(text_size.x + 20.0f * g_scale, text_size.y + 6.0f * g_scale);

    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_active);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.533f, 0.600f, 0.733f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();

    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}


static bool MioLoginVerify(const char* code)
{
    (void)code;
    return true;
}

static bool MioButtonSized(const char* label, float w, float h, float font_size,
                           const ImVec4& bg, const ImVec4& bg_hover, const ImVec4& bg_active)
{
    ImGui::PushFont(g_font, font_size);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_active);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.533f, 0.600f, 0.733f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const bool clicked = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();

    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

static void EnsureHeaderBgTexture();
static void DrawWindowBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding);

static void DrawLoginUI()
{
    if (!g_login_open)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    if (ds.x < 64.0f || ds.y < 64.0f)
        return;

    g_drag_exclude_count = 0;

    const float s = g_scale;
    const float card_w = ImMin(300.0f * s, ds.x - 32.0f * s);
    const float card_h = 130.0f * s;
    const ImVec2 card((ds.x - card_w) * 0.5f, (ds.y - card_h) * 0.5f);
    const float rounding = 12.0f * s;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ds);
    if (g_login_focus_once)
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags lg = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##MioLogin", nullptr, lg);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    EnsureHeaderBgTexture();
    DrawWindowBackground(dl, ImVec2(0.0f, 0.0f), ds, rounding);
    dl->AddRect(ImVec2(0.0f, 0.0f), ds, IM_COL32(187, 187, 187, 255), rounding, 0, 1.0f * s);

    const ImVec2 card_shadow(3.0f * s, 3.0f * s);
    dl->AddRectFilled(card + card_shadow, card + ImVec2(card_w, card_h) + card_shadow,
                      IM_COL32(0, 0, 0, 26), rounding);
    dl->AddRectFilled(card, card + ImVec2(card_w, card_h), IM_COL32(255, 255, 255, 255), rounding);
    dl->AddRect(card, card + ImVec2(card_w, card_h), IM_COL32(0x88, 0x99, 0xbb, 255), rounding, 0, 1.0f * s);

    if (g_drag_exclude_count < 16)
        g_drag_exclude[g_drag_exclude_count++] = ImRect(card, card + ImVec2(card_w, card_h));

    ImGui::PushFont(g_font, 16.0f);

    ImGui::SetCursorScreenPos(ImVec2(card.x + 16.0f * s, card.y + 16.0f * s));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(40, 40, 60, 255));
    ImGui::TextUnformatted("Please Input Auth Code");
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(card.x + 16.0f * s, card.y + 44.0f * s));
    ImGui::SetNextItemWidth(card_w - 32.0f * s);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.93f, 0.93f, 0.93f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.89f, 0.89f, 0.89f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.86f, 0.86f, 0.86f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(40, 40, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.533f, 0.600f, 0.733f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * s, 3.0f * s));
    if (g_login_focus_once)
    {
        ImGui::SetKeyboardFocusHere();
        g_login_focus_once = false;
    }
    const bool enter = ImGui::InputText("##authcode", g_login_code, IM_ARRAYSIZE(g_login_code),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (g_drag_exclude_count < 16)
        g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    const ImVec4 blue(0.565f, 0.722f, 0.902f, 1.0f);
    const ImVec4 blue_hover(0.518f, 0.682f, 0.871f, 1.0f);
    const ImVec4 blue_active(0.463f, 0.635f, 0.835f, 1.0f);
    const float bw = 90.0f * s;
    const float bh = 28.0f * s;
    const float by = card.y + card_h - bh - 16.0f * s;

    ImGui::SetCursorScreenPos(ImVec2(card.x + 40.0f * s, by));
    const bool yes = MioButtonSized("Yes(S)", bw, bh, 16.0f, blue, blue_hover, blue_active);
    if (g_drag_exclude_count < 16)
        g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SetCursorScreenPos(ImVec2(card.x + card_w - 40.0f * s - bw, by));
    const bool no = MioButtonSized("No(C)", bw, bh, 16.0f, blue, blue_hover, blue_active);
    if (g_drag_exclude_count < 16)
        g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (yes || enter)
    {
        if (MioLoginVerify(g_login_code))
            g_login_open = false;
    }
    else if (no)
    {
        g_should_close = true;
    }
}

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
static ImTextureID g_header_bg_tex = (ImTextureID)0;
static int         g_header_bg_w = 0;
static int         g_header_bg_h = 0;
static double      g_header_bg_last_try = -100.0;
static std::string W2U8(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return std::string();
    std::string out((size_t)len, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, nullptr, nullptr);
    return out;
}
static void EnsureHeaderBgTexture()
{
    if (g_header_bg_tex)
        return;
    const double now = ImGui::GetTime();
    if (now - g_header_bg_last_try < 2.0)
        return;
    g_header_bg_last_try = now;
    wchar_t exe_path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring dir(exe_path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.erase(slash + 1);
    std::vector<std::wstring> dirs;
    dirs.push_back(dir);
    wchar_t local[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        dirs.push_back(std::wstring(local) + L"\\Mio\\");
    static const wchar_t* kNames[] = {
        L"MioBackground.png", L"MioBackground.jpg", L"MioBackground.bmp",
        L"background.png", L"background.jpg", L"background.bmp",
    };
    for (const std::wstring& base : dirs)
    {
        for (const wchar_t* name : kNames)
        {
            const std::wstring full = base + name;
            if (::GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            int iw = 0, ih = 0, comp = 0;
            unsigned char* pixels = stbi_load(W2U8(full).c_str(), &iw, &ih, &comp, 4);
            if (!pixels)
                continue;
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            g_header_bg_tex = (ImTextureID)(intptr_t)tex;
            g_header_bg_w = iw;
            g_header_bg_h = ih;
            return;
        }
    }
}
static void DrawWindowBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding)
{
    if (g_header_bg_tex)
    {
        const float bw = p1.x - p0.x;
        const float bh = p1.y - p0.y;
        const float img_ar = (float)g_header_bg_w / (float)g_header_bg_h;
        const float dst_ar = bw / bh;
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        if (img_ar > dst_ar)
        {
            const float f = 0.5f * (dst_ar / img_ar);
            uv0.x = 0.5f - f;
            uv1.x = 0.5f + f;
        }
        else
        {
            const float f = 0.5f * (img_ar / dst_ar);
            uv0.y = 0.5f - f;
            uv1.y = 0.5f + f;
        }
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255), rounding); 
        dl->AddImageRounded(g_header_bg_tex, p0, p1, uv0, uv1, IM_COL32(255, 255, 255, 255), rounding);
        return;
    }
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255), rounding);
}


static HANDLE            g_ctrl_mapping = nullptr;
static MioControlShared* g_ctrl_view = nullptr;
static void MioControlInit()
{
    if (g_ctrl_view)
        return;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(MioControlShared), MIO_CONTROL_SHARED_NAME);
    const bool created = (mapping != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
    if (!mapping)
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MIO_CONTROL_SHARED_NAME);
    if (!mapping)
        return;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MioControlShared));
    if (!view)
    {
        CloseHandle(mapping);
        return;
    }
    g_ctrl_mapping = mapping;
    g_ctrl_view = (MioControlShared*)view;
    if (created || g_ctrl_view->magic != MIO_CONTROL_SHARED_MAGIC)
    {
        g_ctrl_view->magic = MIO_CONTROL_SHARED_MAGIC;
        g_ctrl_view->version = 1;
        g_ctrl_view->flags = 0;
        g_ctrl_view->tick_ms = 0;
    }
}
static void MioControlSetAntiMute(bool on)
{
    if (!g_ctrl_view)
        MioControlInit();
    if (!g_ctrl_view || g_ctrl_view->magic != MIO_CONTROL_SHARED_MAGIC)
        return;
    if (on)
        g_ctrl_view->flags |= MIO_CTRL_FLAG_ANTI_MUTE;
    else
        g_ctrl_view->flags &= ~MIO_CTRL_FLAG_ANTI_MUTE;
    g_ctrl_view->tick_ms = GetTickCount64();
}

static void MioControlHeartbeat()
{
    if (!g_ctrl_view)
        MioControlInit();
    if (!g_ctrl_view || g_ctrl_view->magic != MIO_CONTROL_SHARED_MAGIC)
        return;
    g_ctrl_view->tick_ms = GetTickCount64();
}
static std::wstring MioAntiMuteCfgPath()
{
    wchar_t local[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        return std::wstring();
    std::wstring dir = std::wstring(local) + L"\\Mio";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\antimute.cfg";
}
static bool MioAntiMuteLoad()
{
    const std::wstring path = MioAntiMuteCfgPath();
    if (path.empty())
        return false;
    FILE* f = _wfopen(path.c_str(), L"rt");
    if (!f)
        return false;
    const int c = fgetc(f);
    fclose(f);
    return c == '1';
}
static void MioAntiMuteSave(bool on)
{
    const std::wstring path = MioAntiMuteCfgPath();
    if (path.empty())
        return;
    FILE* f = _wfopen(path.c_str(), L"wt");
    if (!f)
        return;
    fputc(on ? '1' : '0', f);
    fclose(f);
}

static std::string g_status_label = "Debug";
static double g_status_label_last_check = -100.0;
static std::wstring MioIniPrimaryPath()
{
    return L"C:\\Mio.ini";
}
static std::wstring MioIniUserPath()
{
    wchar_t prof[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH))
        return std::wstring();
    return std::wstring(prof) + L"\\Mio.ini";
}
static std::string MioIniTrim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}
static std::string MioIniReadStatus(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rt");
    if (!f)
        return std::string();
    std::string result;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = MioIniTrim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[')
            continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = MioIniTrim(l.substr(0, eq));
        if (_stricmp(key.c_str(), "status") == 0 || _stricmp(key.c_str(), "debug") == 0)
        {
            result = MioIniTrim(l.substr(eq + 1));
            if (!result.empty())
                break;
        }
    }
    fclose(f);
    return result;
}
static bool MioIniWriteDefault(const std::wstring& path)
{
    static const char kDefault[] =
        "[Mio]\r\n"
        "; 状态页「当前工作级别」显示的文字；修改保存后约 2 秒自动生效\r\n"
        "Status=Debug\r\n"
        "; 增益模式：0=只用 TT 进程内采集增益（默认，放大「对方听到的我」）\r\n"
        ";           1=额外启动外部放音增益（虚拟声卡环路，放大「我听到的对方」）\r\n"
        "OuterGain=0\r\n";
    FILE* f = _wfopen(path.c_str(), L"wt");
    if (!f)
        return false;
    fwrite(kDefault, 1, sizeof(kDefault) - 1, f);
    fclose(f);
    return true;
}
static void MioIniEnsureCreated()
{
    std::wstring primary = MioIniPrimaryPath();
    if (::GetFileAttributesW(primary.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!MioIniWriteDefault(primary))
        {
            std::wstring user = MioIniUserPath();
            if (!user.empty() && ::GetFileAttributesW(user.c_str()) == INVALID_FILE_ATTRIBUTES)
                MioIniWriteDefault(user);
        }
    }
}
static std::string MioIniReadKey(const std::wstring& path, const char* want_key)
{
    FILE* f = _wfopen(path.c_str(), L"rt");
    if (!f)
        return std::string();
    std::string result;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = MioIniTrim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[')
            continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = MioIniTrim(l.substr(0, eq));
        if (_stricmp(key.c_str(), want_key) == 0)
        {
            result = MioIniTrim(l.substr(eq + 1));
            if (!result.empty())
                break;
        }
    }
    fclose(f);
    return result;
}
static std::string MioIniReadKbps(const std::wstring& path)
{
    return MioIniReadKey(path, "kbps");
}

static int MioIniReadInt(const char* key, int def)
{
    std::string v = MioIniReadKey(MioIniPrimaryPath(), key);
    if (v.empty())
        v = MioIniReadKey(MioIniUserPath(), key);
    if (v.empty())
        return def;
    int n = 0;
    bool any = false;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const char c = v[i];
        if (c >= '0' && c <= '9')
        {
            n = n * 10 + (c - '0');
            any = true;
            if (n > 1000000)
                break;
        }
        else
        {
            break;
        }
    }
    return any ? n : def;
}

static bool MioOuterGainEnabled()
{
    return MioIniReadInt("OuterGain", 0) != 0;
}

static bool HexNibble(char ch, int& v)
{
    if (ch >= '0' && ch <= '9') { v = ch - '0'; return true; }
    if (ch >= 'a' && ch <= 'f') { v = ch - 'a' + 10; return true; }
    if (ch >= 'A' && ch <= 'F') { v = ch - 'A' + 10; return true; }
    return false;
}
static void ParseTitleGrad(const std::string& v)
{
    ImVec4 out[8];
    int n = 0;
    size_t pos = 0;
    while (n < 8)
    {
        const size_t comma = v.find(',', pos);
        std::string tok = MioIniTrim(v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!tok.empty() && tok[0] == '#')
            tok = tok.substr(1);
        if (tok.size() != 6)
            return;
        int hx[6] = {};
        for (int i = 0; i < 6; ++i)
            if (!HexNibble(tok[i], hx[i]))
                return;
        out[n].x = (float)((hx[0] << 4) | hx[1]) / 255.0f;
        out[n].y = (float)((hx[2] << 4) | hx[3]) / 255.0f;
        out[n].z = (float)((hx[4] << 4) | hx[5]) / 255.0f;
        out[n].w = 1.0f;
        ++n;
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    if (n == 1)
    {
        out[1] = out[0];
        n = 2;
    }
    if (n >= 2)
    {
        for (int i = 0; i < n; ++i)
            g_grad_stops[i] = out[i];
        g_grad_stop_count = n;
    }
}

static void MioBoostKbpsSyncTick()
{
    std::string kv = MioIniReadKbps(MioIniPrimaryPath());
    if (kv.empty())
        kv = MioIniReadKbps(MioIniUserPath());
    int kbps = 400; 
    bool any = false;
    int parsed = 0;
    for (size_t i = 0; i < kv.size(); ++i)
    {
        if (kv[i] >= '0' && kv[i] <= '9')
        {
            parsed = parsed * 10 + (kv[i] - '0');
            any = true;
            if (parsed > 20000)
                break;
        }
        else
        {
            break;
        }
    }
    if (any && parsed <= 20000)
        kbps = parsed;
    static int last_synced = -1;
    if (kbps == last_synced)
        return;
    g_room_kbps.SetBoostTarget(kbps); 
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    std::wstring dir = std::wstring(base) + L"\\Mio";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\boost-target.txt";
    FILE* f = _wfopen(path.c_str(), L"wt");
    if (!f)
        return;
    fprintf(f, "%d", kbps);
    fclose(f);
    last_synced = kbps;
}
static void MioStatusLabelTick()
{
    const double now = ImGui::GetTime();
    if (now - g_status_label_last_check < 2.0)
        return;
    g_status_label_last_check = now;
    MioIniEnsureCreated();
    std::string v = MioIniReadStatus(MioIniPrimaryPath());
    if (v.empty())
        v = MioIniReadStatus(MioIniUserPath());
    if (v.empty())
        v = "Debug";
    if (v.size() > 64)
        v.resize(64);
    g_status_label = v;
    MioBoostKbpsSyncTick(); 
  
    static std::string last_grad_val;
    std::string grad_v = MioIniReadKey(MioIniPrimaryPath(), "TitleGrad");
    if (grad_v.empty())
        grad_v = MioIniReadKey(MioIniUserPath(), "TitleGrad");
    if (grad_v != last_grad_val)
    {
        last_grad_val = grad_v;
        g_grad_stop_count = 0;
        if (!grad_v.empty())
            ParseTitleGrad(grad_v);
    }
}

static void DrawMioUI(float time)
{
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::GetTime() - g_last_tt_check >= 0.5)
    {
        g_tt_active = IsTTVoiceEntertainmentRunning();
        g_last_tt_check = ImGui::GetTime();
    }

    EnsureHeaderBgTexture();
    MioStatusLabelTick(); 


    g_room_kbps.Tick();


    mio_audio::Level().Update();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_scale, 8.0f * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * g_scale, 4.0f * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("Mio Client", nullptr, flags);
    g_drag_exclude_count = 0;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1 = p0 + io.DisplaySize;
    const float rounding = 12.0f * g_scale;
    DrawWindowBackground(dl, p0, p1, rounding); 
    dl->AddRect(p0, p1, IM_COL32(187, 187, 187, 255), rounding, 0, 1.0f * g_scale);

    ImGui::PushFont(g_font, 20.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(40, 40, 60, 255));

    char time_val[48];
    const long long total = 46 + (long long)time;
    snprintf(time_val, sizeof(time_val), "%03lld天：%02lld时：%02lld分：%02lld秒",
             total / 86400, (total % 86400) / 3600, (total % 3600) / 60, total % 60);

    
    ImGui::PushFont(g_font_bold, 20.0f);
    ImGui::TextUnformatted("Mio Client");
    ImGui::PopFont();


    const float grad_y = ImGui::GetItemRectMax().y + 6.0f * g_scale;
    const float grad_x0 = p0.x + 6.0f * g_scale;
    const float grad_x1 = p1.x - 6.0f * g_scale;
    const float grad_h = 3.0f * g_scale;

    
    const float vline_x = 6.0f * g_scale;
    const float vline_top = grad_y + grad_h;
    const float vline_bottom = io.DisplaySize.y - 2.0f * g_scale;
    const ImU32 vline_col = IM_COL32(180, 180, 180, 255);
    dl->AddLine(ImVec2(p0.x + vline_x, p0.y + vline_top), ImVec2(p0.x + vline_x, p0.y + vline_bottom), vline_col, 1.0f * g_scale);
    dl->AddLine(ImVec2(p1.x - vline_x, p0.y + vline_top), ImVec2(p1.x - vline_x, p0.y + vline_bottom), vline_col, 1.0f * g_scale);

    const int grad_segs = 48;
    for (int i = 0; i < grad_segs; ++i)
    {
        const float gx0 = grad_x0 + (grad_x1 - grad_x0) * (float)i / grad_segs;
        const float gx1 = grad_x0 + (grad_x1 - grad_x0) * (float)(i + 1) / grad_segs;
        dl->AddRectFilled(ImVec2(gx0, grad_y), ImVec2(gx1, grad_y + grad_h), Gradient4((float)i / (grad_segs - 1)));
    }


    ImGui::Dummy(ImVec2(0.0f, 10.0f * g_scale));


    const ImVec4 blue(0.8118f, 0.8118f, 0.8118f, 1.0f);
    const ImVec4 blue_hover(0.76f, 0.76f, 0.76f, 1.0f);
    const ImVec4 blue_active(0.71f, 0.71f, 0.71f, 1.0f);
    const ImVec4 highlight(1.00f, 0.8824f, 1.00f, 1.0f);

    const char* labels[] = { "状态", "监听", "关于", "房间码率" };
    const float tab_gap = 10.0f * g_scale;
    for (int i = 0; i < IM_ARRAYSIZE(labels); ++i)
    {
        const bool active = (g_active_tab == i);
        const ImVec2 text_size = ImGui::CalcTextSize(labels[i]);
        const ImVec2 size(text_size.x + 12.0f * g_scale, text_size.y + 4.0f * g_scale);

        ImGui::PushStyleColor(ImGuiCol_Button, active ? highlight : blue);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? highlight : blue_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? highlight : blue_active);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(40, 40, 60, 255));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.533f, 0.600f, 0.733f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

        if (ImGui::Button(labels[i], size))
            g_active_tab = i;
        g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        if (i + 1 < IM_ARRAYSIZE(labels))
            ImGui::SameLine(0.0f, tab_gap);
    }

    {
        const float tabs_sep_y = ImGui::GetCursorScreenPos().y + 2.5f * g_scale;
        dl->AddLine(ImVec2(p0.x + 6.0f * g_scale, tabs_sep_y), ImVec2(p1.x - 6.0f * g_scale, tabs_sep_y), IM_COL32(180, 180, 180, 255), 1.0f * g_scale);
        ImGui::Dummy(ImVec2(0.0f, 8.0f * g_scale));
    }
    if (g_active_tab == 0)
    {
        if (g_tt_active)
            RowWithGradientColors("声卡桥接驱动：", "已启动",
                ImVec4(1.00f, 0.2549f, 0.4235f, 1.0f), ImVec4(1.00f, 0.2941f, 0.1686f, 1.0f));
        else
            RowWithGradientColors("声卡桥接驱动：", "未启动",
                ImVec4(0.8980f, 0.8941f, 0.8863f, 1.0f), ImVec4(0.7529f, 0.7529f, 0.7529f, 1.0f));

        {
            const float rows_sep_y = ImGui::GetCursorScreenPos().y + 5.0f * g_scale;
            dl->AddLine(ImVec2(p0.x + 6.0f * g_scale, rows_sep_y), ImVec2(p1.x - 6.0f * g_scale, rows_sep_y), IM_COL32(180, 180, 180, 255), 1.0f * g_scale);
            ImGui::Dummy(ImVec2(0.0f, 2.0f * g_scale));
        }
    
        RowWithRainbow("当前工作级别：", g_status_label.c_str(), (float)ImGui::GetTime());
    }
    else if (g_active_tab == 1)
    {
        if (ImGui::GetTime() - g_last_device_refresh > 1.0)
        {
            CollectActiveDevices(true, g_active_input);
            CollectActiveDevices(false, g_active_output);
            g_last_device_refresh = ImGui::GetTime();
        }

        ImGui::BeginChild("##ListenList", ImVec2(0.0f, 78.0f * g_scale), false);
        std::string input_line = "输入设备：";
        for (size_t i = 0; i < g_active_input.size(); ++i)
        {
            if (i > 0)
                input_line += " / ";
            char buf[512];
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, g_active_input[i].c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
            if (len > 0)
                input_line += buf;
        }
        if (g_active_input.empty())
            input_line += "无";
        ImGui::TextUnformatted(input_line.c_str());

    
        const float io_sep_y = ImGui::GetCursorScreenPos().y + ImGui::GetFontSize() * 0.5f;
        dl->AddLine(ImVec2(p0.x + 6.0f * g_scale, io_sep_y), ImVec2(p1.x - 6.0f * g_scale, io_sep_y), IM_COL32(180, 180, 180, 255), 1.0f * g_scale);
        ImGui::Dummy(ImVec2(0.0f, 8.0f * g_scale));

        std::string output_line = "输出设备：";
        for (size_t i = 0; i < g_active_output.size(); ++i)
        {
            if (i > 0)
                output_line += " / ";
            char buf[512];
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, g_active_output[i].c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
            if (len > 0)
                output_line += buf;
        }
        if (g_active_output.empty())
            output_line += "无";
        ImGui::TextUnformatted(output_line.c_str());

        const float out_sep_y = ImGui::GetCursorScreenPos().y + ImGui::GetFontSize() * 0.5f;
        dl->AddLine(ImVec2(p0.x + 6.0f * g_scale, out_sep_y), ImVec2(p1.x - 6.0f * g_scale, out_sep_y), IM_COL32(180, 180, 180, 255), 1.0f * g_scale);
        ImGui::Dummy(ImVec2(0.0f, 6.0f * g_scale));
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * g_scale));
        mio_audio::Level().Draw(g_scale);
    }
    else
    {
        if (g_active_tab == 3)
        {
            const float kbps_top = ImGui::GetCursorScreenPos().y;
            g_room_kbps.Draw(g_scale, g_tt_active, g_font, g_font_bold);
            const float kbps_bottom = ImGui::GetCursorScreenPos().y;
            if (g_drag_exclude_count < 16)
                g_drag_exclude[g_drag_exclude_count++] =
                    ImRect(ImVec2(p0.x, kbps_top), ImVec2(p1.x, kbps_bottom));
        }
        else
        {
            ImGui::TextUnformatted("开发者|发光的神");
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(180, 180, 180, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();

    ImGui::PushFont(g_font, 16.0f);
    const float btn_text_h = ImGui::CalcTextSize("退出").y;
    const float exit_w = ImGui::CalcTextSize("退出").x + 20.0f * g_scale;
    const float anti_text_w = ImGui::CalcTextSize("防闭麦").x;
    ImGui::PopFont();
    const float anti_w = anti_text_w + 20.0f * g_scale;
    const float gap = 8.0f * g_scale;
    const float group_w = anti_w + gap + exit_w;
    const float content_h = io.DisplaySize.y - 16.0f * g_scale;
    const float btn_h = btn_text_h + 6.0f * g_scale;
    const float push = content_h - ImGui::GetCursorPosY() - btn_h;
    if (push > 0.0f)
        ImGui::Dummy(ImVec2(0.0f, push));

    ImGui::PushFont(g_font, 16.0f);
    {

        const float fs = 16.0f;
        const char* pfx = "运行时间：";
        const float pfx_w = TextWidth(g_font, fs, pfx);
        const ImVec2 tpos = ImGui::GetCursorScreenPos();
        dl->AddText(g_font, fs, tpos, IM_COL32(40, 40, 60, 255), pfx);
        DrawRainbowText(dl, g_font, fs, ImVec2(tpos.x + pfx_w, tpos.y), time_val, (float)ImGui::GetTime());
        ImGui::Dummy(ImVec2(pfx_w + TextWidth(g_font, fs, time_val), ImGui::GetTextLineHeight()));
    }
    const float time_w = ImGui::GetItemRectSize().x;
    ImGui::PopFont();

    ImGui::SameLine(0.0f, ImGui::GetContentRegionAvail().x - group_w - time_w - 24.0f * g_scale);

    ImGui::PushFont(g_font, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.72f, 0.72f, 0.72f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
    ImGui::Checkbox("防闭麦", &g_anti_mute);
    if (g_anti_mute != g_anti_mute_sent)
    {
        MioControlSetAntiMute(g_anti_mute);
        MioAntiMuteSave(g_anti_mute);
        g_anti_mute_sent = g_anti_mute;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine(0.0f, gap);

    if (MioButton("退出", blue, blue_hover, blue_active, 16.0f))
        g_should_close = true;
    g_drag_exclude[g_drag_exclude_count++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    const float sep_y = ImGui::GetItemRectMax().y + 6.0f * g_scale;
    dl->AddLine(ImVec2(p0.x + 6.0f * g_scale, sep_y), ImVec2(p1.x - 6.0f * g_scale, sep_y), IM_COL32(180, 180, 180, 255), 1.0f * g_scale);

    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar(3);
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_NCHITTEST:
    {
        const LRESULT res = ::DefWindowProcW(hWnd, msg, wParam, lParam);
        if (res == HTCLIENT)
        {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);
            for (int i = 0; i < g_drag_exclude_count; ++i)
            {
                if (g_drag_exclude[i].Contains(ImVec2((float)pt.x, (float)pt.y)))
                    return res;
            }
            return HTCAPTION;
        }
        return res;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_Width = LOWORD(lParam);
            g_Height = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            g_should_close = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        ::DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool CreateDeviceWGL(HWND hWnd)
{
    HDC hDc = ::GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = { 0 };
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;

    const int pf = ::ChoosePixelFormat(hDc, &pfd);
    if (pf == 0)
        return false;
    if (::SetPixelFormat(hDc, pf, &pfd) == FALSE)
        return false;
    ::ReleaseDC(hWnd, hDc);

    g_hDC = ::GetDC(hWnd);
    HGLRC tempRC = wglCreateContext(g_hDC);
    if (!tempRC)
        return false;
    if (!wglMakeCurrent(g_hDC, tempRC))
    {
        wglDeleteContext(tempRC);
        return false;
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(0x821B, &major);
    glGetIntegerv(0x821C, &minor);
    const char* version = (const char*)glGetString(GL_VERSION);
    if (major == 0 && minor == 0)
        sscanf_s(version, "%d.%d", &major, &minor);
    const GLuint gl_version = (GLuint)(major * 100 + minor * 10);

    if (gl_version >= 300)
    {
        g_hRC = tempRC;
        return true;
    }

    typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    const PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    const int attribs[] =
    {
        0x2091, 3,
        0x2092, 0,
        0x9126, 0x0001,
        0
    };
    HGLRC newRC = wglCreateContextAttribsARB ? wglCreateContextAttribsARB(g_hDC, 0, attribs) : nullptr;
    if (newRC)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        g_hRC = newRC;
        wglMakeCurrent(g_hDC, g_hRC);
    }
    else
    {
        g_hRC = tempRC;
    }
    return true;
}

static void CleanupDeviceWGL(HWND hWnd)
{
    wglMakeCurrent(nullptr, nullptr);
    if (g_hDC)
        ::ReleaseDC(hWnd, g_hDC);
}

static ImFont* LoadCjkFont(const char* const* paths, int count, float size)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    for (int i = 0; i < count; ++i)
    {
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(paths[i], size, nullptr, ranges))
            return font;
    }
    return nullptr;
}

static ImFont* LoadGlobalFont(const char* latin_path, const char* cjk_path, float size)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.MergeMode = false;
    ImFont* font = io.Fonts->AddFontFromFileTTF(latin_path, size, &cfg, io.Fonts->GetGlyphRangesDefault());
    if (!font)
        return nullptr;
    cfg.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(cjk_path, size, &cfg, io.Fonts->GetGlyphRangesChineseFull());
    return font;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    g_scale *= 0.9f; 

    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"MioWindowClass", nullptr };
    wc.hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)::LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_LAYERED, wc.lpszClassName, L"Mio Client",
        WS_POPUP,
        200, 200,
        (int)(390 * g_scale), (int)(240 * g_scale), 
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        return 1;

    WNDCLASSEXW status_wc = { sizeof(status_wc), 0, StatusOverlayWndProc, 0L, 0L,
                              hInstance, nullptr, nullptr, nullptr, nullptr,
                              L"MioStatusOverlayClass", nullptr };
    ::RegisterClassExW(&status_wc);
    g_status_overlay = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        status_wc.lpszClassName, L"MioStatusOverlay", WS_POPUP,
        0, 0, 320, 96, hwnd, nullptr, hInstance, nullptr);
    if (!g_status_overlay)
    {
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(status_wc.lpszClassName, hInstance);
        return 1;
    }

    if (!CreateDeviceWGL(hwnd))
    {
        CleanupDeviceWGL(hwnd);
        ::DestroyWindow(hwnd);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    const int rw = (int)(390 * g_scale) + 1;
    const int rh = (int)(240 * g_scale) + 1;
    const int radius = (int)(12 * g_scale);
    ::SetWindowRgn(hwnd, ::CreateRoundRectRgn(0, 0, rw, rh, radius * 2, radius * 2), TRUE);
    ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA); 

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_scale);
    style.FontScaleDpi = g_scale;
    style.WindowBorderSize = 0.0f;

    const float base_size = 20.0f * g_scale;
    static const char* bold_candidates[] = {
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
    };
    g_font = LoadCjkFont(bold_candidates, 4, base_size);
    g_font_bold = g_font;
    if (!g_font)
        g_font = io.Fonts->AddFontDefault();
    if (!g_font_bold)
        g_font_bold = g_font;

    g_tt_active = IsTTVoiceEntertainmentRunning();
    if (!g_login_open)
    {
        UpdateStatusOverlay(g_status_overlay, g_tt_active);
        g_status_overlay_state = g_tt_active;
        g_status_overlay_has_state = true;
    }

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init();
    g_imgui_ready = true;

  
    mio_zego::ExtractBundledProbe();
   
    MioControlInit();
    g_anti_mute = MioAntiMuteLoad();
    MioControlSetAntiMute(g_anti_mute);
    g_anti_mute_sent = g_anti_mute;

    if (MioOuterGainEnabled())
        mio_gain::AutoStart();
    else
        mio_gain::Log("gain: 外部放音增益关闭（OuterGain=0）—— 增益走 TT 进程内采集 AGC");

    mio_integrity::Verify();

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
     
        {
            static DWORD64 last_hb_ms = 0;
            const DWORD64 now_hb_ms = ::GetTickCount64();
            if (now_hb_ms - last_hb_ms >= 1000)
            {
                last_hb_ms = now_hb_ms;
                MioControlHeartbeat();
            }
        }
        {
            static bool overlay_visible = false;
            if (g_login_open)
            {
                if (overlay_visible && g_status_overlay)
                {
                    ::ShowWindow(g_status_overlay, SW_HIDE);
                    overlay_visible = false;
                }
                g_status_overlay_has_state = false;
            }
            else
            {
                static DWORD64 last_overlay_ms = 0;
                const DWORD64 now_ov_ms = ::GetTickCount64();
                const bool ov_changed = (!g_status_overlay_has_state || g_status_overlay_state != g_tt_active);
                if (ov_changed || now_ov_ms - last_overlay_ms >= 40)
                {
                    last_overlay_ms = now_ov_ms;
                    UpdateStatusOverlay(g_status_overlay, g_tt_active);
                    g_status_overlay_state = g_tt_active;
                    g_status_overlay_has_state = true;
                    overlay_visible = true;
                }
            }
        }
        if (::IsIconic(hwnd))
        {
            ::Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_login_open)
        {
            DrawLoginUI();
        }
        else
        {
            DrawMioUI((float)ImGui::GetTime());
        }

        if (g_should_close)
            done = true;

        ImGui::Render();
        glViewport(0, 0, g_Width, g_Height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(g_hDC);
    }


    mio_gain::Unload();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_hRC)
        wglDeleteContext(g_hRC);
    CleanupDeviceWGL(hwnd);
    if (g_status_overlay)
    {
        ::DestroyWindow(g_status_overlay);
        g_status_overlay = nullptr;
    }
    ::UnregisterClassW(status_wc.lpszClassName, hInstance);
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
