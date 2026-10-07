#pragma once
// ---------------------------------------------------------------------------
// SystemMicCapture.h —— Mio 直接调用系统麦克风（WASAPI 共享模式采集）
// 默认采集端点，后台线程循环读包 → RMS/峰值（0..1000，与 TT 载荷同刻度）。
// 监听页电平条首选数据源；拿不到数据时上层自动回退 TT 共享内存。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdint>
#include <cmath>
namespace mio_sysmic {
struct State
{
    volatile LONG  seq;        // 奇=写入中
    volatile LONG  rms_milli;
    volatile LONG  peak_milli;
    volatile LONG64 tick_ms;
    volatile LONG  running;    // 1=采集会话进行中
    volatile LONG  started;    // 惰性启动标记
};
inline State& S()
{
    static State s = {};
    return s;
}
inline bool IsRunning()
{
    return S().running == 1;
}
inline void Publish(State& st, unsigned int rm, unsigned int pm)
{
    st.seq++;
    st.rms_milli = (LONG)rm;
    st.peak_milli = (LONG)pm;
    st.tick_ms = (LONG64)::GetTickCount64();
    st.seq++;
}
// 打开一次采集会话并持续读包；返回表示会话结束（失败/设备失效），外层重试
inline void CaptureSession(State& st)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mix = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr) && enumerator)
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (SUCCEEDED(hr) && device)
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (SUCCEEDED(hr) && client)
        hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && client && mix)
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
    if (SUCCEEDED(hr) && client)
        hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr) && client && capture)
        hr = client->Start();
    if (FAILED(hr))
    {
        if (capture) capture->Release();
        if (client) client->Release();
        if (mix) ::CoTaskMemFree(mix);
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        return;
    }
    st.running = 1;
    const bool is_float = (mix->wBitsPerSample == 32);
    const bool is_16 = (mix->wBitsPerSample == 16);
    const unsigned int channels = mix->nChannels > 0 ? (unsigned int)mix->nChannels : 1;
    for (;;)
    {
        ::Sleep(10);
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet)))
        {
            Publish(st, 0, 0);
            break;
        }
        while (packet != 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            {
                packet = 0;
                break;
            }
            if (frames > 0 && data)
            {
                unsigned long long sum = 0;
                int peak = 0;
                const unsigned int samples = frames * channels;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                {
                    if (is_float)
                    {
                        const float* f = (const float*)data;
                        for (unsigned int i = 0; i < samples; ++i)
                        {
                            long v = (long)(f[i] * 32767.0f);
                            if (v > 32767) v = 32767;
                            if (v < -32768) v = -32768;
                            const long av = v < 0 ? -v : v;
                            if (av > peak) peak = (int)av;
                            sum += (unsigned long long)((long long)v * (long long)v);
                        }
                    }
                    else if (is_16)
                    {
                        const short* s = (const short*)data;
                        for (unsigned int i = 0; i < samples; ++i)
                        {
                            const int v = s[i];
                            const int av = v < 0 ? -v : v;
                            if (av > peak) peak = av;
                            sum += (unsigned long long)((long long)v * (long long)v);
                        }
                    }
                }
                unsigned int rm = 0;
                if (samples > 0)
                {
                    const double rms = sqrt((double)sum / (double)samples);
                    rm = (unsigned int)(rms * 1000.0 / 32768.0 + 0.5);
                }
                unsigned int pm = (unsigned int)((unsigned long long)peak * 1000 / 32768);
                if (rm > 1000) rm = 1000;
                if (pm > 1000) pm = 1000;
                Publish(st, rm, pm);
            }
            capture->ReleaseBuffer(frames);
            if (FAILED(capture->GetNextPacketSize(&packet)))
            {
                packet = 0;
                break;
            }
        }
    }
    st.running = 0;
    capture->Release();
    client->Release();
    if (mix) ::CoTaskMemFree(mix);
    device->Release();
    enumerator->Release();
}
inline bool TtCaptureActive()
{
    HANDLE m = ::OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\MioAudioLevel_v1");
    if (!m)
        return false;
    void* v = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 32);
    if (!v)
    {
        ::CloseHandle(m);
        return false;
    }
    const unsigned long long tick = *(volatile unsigned long long*)((unsigned char*)v + 24);
    ::UnmapViewOfFile(v);
    ::CloseHandle(m);
    return tick != 0 && (::GetTickCount64() - tick) <= 2000;
}
inline DWORD WINAPI ThreadProc(LPVOID)
{
    const HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
        return 0;
    State& st = S();
    for (;;)
    {
        if (TtCaptureActive())
        {
            st.running = 0;
            ::Sleep(1000); // TT 正在采集：让出 kX 设备，避免双采导致声音一顿一顿
            continue;
        }
        CaptureSession(st);
        st.running = 0;
        ::Sleep(3000); // 设备切换/失效后自动重开
    }
    return 0;
}
inline void EnsureStarted()
{
    State& st = S();
    if (st.started)
        return;
    st.started = 1;
    HANDLE t = ::CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    if (t)
        ::CloseHandle(t);
}
// 读一帧电平；数据新鲜（<1 秒）返回 true
inline bool Sample(unsigned int& rms_milli, unsigned int& peak_milli)
{
    State& st = S();
    const LONG s1 = st.seq;
    if (s1 & 1)
        return false;
    const LONG rms = st.rms_milli;
    const LONG peak = st.peak_milli;
    const LONG64 tick = st.tick_ms;
    if (st.seq != s1)
        return false;
    if (tick == 0)
        return false;
    if (::GetTickCount64() - (unsigned long long)tick > 1000)
        return false;
    rms_milli = (unsigned int)rms;
    peak_milli = (unsigned int)peak;
    return true;
}
} // namespace mio_sysmic
