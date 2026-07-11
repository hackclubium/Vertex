#pragma once

#include <windows.h>
#include <string>

class Win32MediaPlayer {
public:
    Win32MediaPlayer();
    ~Win32MediaPlayer();

    bool Load(HWND owner, const std::string& url, bool hasVideo, bool autoplay);
    void SetRect(float x, float y, float w, float h);
    void Play();
    void Pause();
    void Stop();
    const std::string& Url() const { return m_url; }
    bool HasVideo() const { return m_hasVideo; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::string m_url;
    bool m_hasVideo = false;
};
