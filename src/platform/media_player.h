#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
using PlatformMediaOwner = HWND;
#else
using PlatformMediaOwner = void*;
#endif

class PlatformMediaPlayer {
public:
    PlatformMediaPlayer();
    ~PlatformMediaPlayer();

    bool Load(PlatformMediaOwner owner, const std::string& url, bool hasVideo, bool autoplay);
    void SetRect(float x, float y, float w, float h);
    void Play();
    void Pause();
    void Stop();
    void SetCurrentTime(double seconds);
    double CurrentTime() const;
    double Duration() const;
    void SetVolume(double volume);
    double Volume() const;
    void SetMuted(bool muted);
    bool Muted() const;
    bool Paused() const;
    const std::string& Url() const { return m_url; }
    bool HasVideo() const { return m_hasVideo; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::string m_url;
    bool m_hasVideo = false;
    bool m_muted = false;
    double m_volume = 1.0;
};
