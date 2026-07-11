#ifdef __linux__
#include "platform/media_player.h"

#include <algorithm>

struct PlatformMediaPlayer::Impl {};

PlatformMediaPlayer::PlatformMediaPlayer() : m_impl(new Impl) {}
PlatformMediaPlayer::~PlatformMediaPlayer() { delete m_impl; }

bool PlatformMediaPlayer::Load(PlatformMediaOwner, const std::string& url, bool hasVideo, bool autoplay) {
    m_url = url;
    m_hasVideo = hasVideo;
    (void)autoplay;
    return !url.empty();
}
void PlatformMediaPlayer::SetRect(float, float, float, float) {}
void PlatformMediaPlayer::Play() {}
void PlatformMediaPlayer::Pause() {}
void PlatformMediaPlayer::Stop() {}
void PlatformMediaPlayer::SetCurrentTime(double seconds) { (void)seconds; }
double PlatformMediaPlayer::CurrentTime() const { return 0.0; }
double PlatformMediaPlayer::Duration() const { return 0.0; }
void PlatformMediaPlayer::SetVolume(double volume) { m_volume = std::max(0.0, std::min(1.0, volume)); }
double PlatformMediaPlayer::Volume() const { return m_volume; }
void PlatformMediaPlayer::SetMuted(bool muted) { m_muted = muted; }
bool PlatformMediaPlayer::Muted() const { return m_muted; }
bool PlatformMediaPlayer::Paused() const { return true; }

#endif
