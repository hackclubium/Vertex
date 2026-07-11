#ifdef _WIN32
#include "platform/media_win32.h"

#include <dshow.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>

struct Win32MediaPlayer::Impl {
    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IVideoWindow* video = nullptr;
    IMediaSeeking* seeking = nullptr;
    IBasicAudio* audio = nullptr;
};

static std::wstring MediaToWide(const std::string& s) {
    if (s.rfind("file://", 0) == 0) {
        std::string path = s.substr(7);
        std::replace(path.begin(), path.end(), '/', '\\');
        int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
        std::wstring out(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), out.data(), n);
        return out;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

Win32MediaPlayer::Win32MediaPlayer() : m_impl(new Impl) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

Win32MediaPlayer::~Win32MediaPlayer() {
    Stop();
    if (m_impl->video) { m_impl->video->put_Visible(OAFALSE); m_impl->video->put_Owner(0); m_impl->video->Release(); }
    if (m_impl->audio) m_impl->audio->Release();
    if (m_impl->seeking) m_impl->seeking->Release();
    if (m_impl->control) m_impl->control->Release();
    if (m_impl->graph) m_impl->graph->Release();
    delete m_impl;
}

bool Win32MediaPlayer::Load(HWND owner, const std::string& url, bool hasVideo, bool autoplay) {
    Stop();
    if (m_impl->video) { m_impl->video->put_Visible(OAFALSE); m_impl->video->put_Owner(0); m_impl->video->Release(); m_impl->video = nullptr; }
    if (m_impl->audio) { m_impl->audio->Release(); m_impl->audio = nullptr; }
    if (m_impl->seeking) { m_impl->seeking->Release(); m_impl->seeking = nullptr; }
    if (m_impl->control) { m_impl->control->Release(); m_impl->control = nullptr; }
    if (m_impl->graph) { m_impl->graph->Release(); m_impl->graph = nullptr; }

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_impl->graph)))) return false;
    if (FAILED(m_impl->graph->QueryInterface(IID_PPV_ARGS(&m_impl->control)))) return false;

    std::wstring wide = MediaToWide(url);
    if (FAILED(m_impl->graph->RenderFile(wide.c_str(), nullptr))) return false;
    m_impl->graph->QueryInterface(IID_PPV_ARGS(&m_impl->seeking));
    m_impl->graph->QueryInterface(IID_PPV_ARGS(&m_impl->audio));

    m_url = url;
    m_hasVideo = hasVideo;
    if (hasVideo && SUCCEEDED(m_impl->graph->QueryInterface(IID_PPV_ARGS(&m_impl->video)))) {
        m_impl->video->put_Owner((OAHWND)owner);
        m_impl->video->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        m_impl->video->put_Visible(OATRUE);
    }
    if (autoplay) Play();
    SetVolume(m_volume);
    SetMuted(m_muted);
    return true;
}

void Win32MediaPlayer::SetRect(float x, float y, float w, float h) {
    if (m_impl->video)
        m_impl->video->SetWindowPosition((long)x, (long)y, (long)std::max(1.f, w), (long)std::max(1.f, h));
}

void Win32MediaPlayer::Play() { if (m_impl->control) m_impl->control->Run(); }
void Win32MediaPlayer::Pause() { if (m_impl->control) m_impl->control->Pause(); }
void Win32MediaPlayer::Stop() { if (m_impl->control) m_impl->control->Stop(); }

void Win32MediaPlayer::SetCurrentTime(double seconds) {
    if (!m_impl->seeking) return;
    LONGLONG pos = (LONGLONG)(std::max(0.0, seconds) * 10000000.0);
    m_impl->seeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
}

double Win32MediaPlayer::CurrentTime() const {
    if (!m_impl->seeking) return 0.0;
    LONGLONG pos = 0;
    return SUCCEEDED(m_impl->seeking->GetCurrentPosition(&pos)) ? (double)pos / 10000000.0 : 0.0;
}

double Win32MediaPlayer::Duration() const {
    if (!m_impl->seeking) return 0.0;
    LONGLONG dur = 0;
    return SUCCEEDED(m_impl->seeking->GetDuration(&dur)) ? (double)dur / 10000000.0 : 0.0;
}

void Win32MediaPlayer::SetVolume(double volume) {
    m_volume = std::max(0.0, std::min(1.0, volume));
    if (!m_impl->audio || m_muted) return;
    long db = m_volume <= 0.0 ? -10000 : (long)(2000.0 * std::log10(m_volume));
    m_impl->audio->put_Volume(std::max(-10000L, std::min(0L, db)));
}

double Win32MediaPlayer::Volume() const { return m_volume; }

void Win32MediaPlayer::SetMuted(bool muted) {
    m_muted = muted;
    if (!m_impl->audio) return;
    if (m_muted) m_impl->audio->put_Volume(-10000);
    else SetVolume(m_volume);
}

bool Win32MediaPlayer::Paused() const {
    if (!m_impl->control) return true;
    OAFilterState state = State_Stopped;
    return FAILED(m_impl->control->GetState(0, &state)) || state != State_Running;
}

#endif
