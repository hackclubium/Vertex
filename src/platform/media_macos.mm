#ifdef __APPLE__
#include "platform/media_player.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#include <algorithm>

struct PlatformMediaPlayer::Impl {
    AVPlayer* player = nil;
    AVPlayerLayer* layer = nil;
    NSView* owner = nil;
};

static NSString* MediaToNSString(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}

PlatformMediaPlayer::PlatformMediaPlayer() : m_impl(new Impl) {}

PlatformMediaPlayer::~PlatformMediaPlayer() {
    Stop();
    if (m_impl->layer) [m_impl->layer removeFromSuperlayer];
    [m_impl->player release];
    [m_impl->layer release];
    delete m_impl;
}

bool PlatformMediaPlayer::Load(PlatformMediaOwner owner, const std::string& url, bool hasVideo, bool autoplay) {
    Stop();
    if (m_impl->layer) { [m_impl->layer removeFromSuperlayer]; [m_impl->layer release]; m_impl->layer = nil; }
    [m_impl->player release];
    m_impl->player = nil;

    NSURL* nsUrl = nil;
    NSString* str = MediaToNSString(url);
    if ([str hasPrefix:@"file://"]) nsUrl = [NSURL fileURLWithPath:[str substringFromIndex:7]];
    else nsUrl = [NSURL URLWithString:str];
    if (!nsUrl) return false;

    m_impl->player = [[AVPlayer alloc] initWithURL:nsUrl];
    if (!m_impl->player) return false;
    m_url = url;
    m_hasVideo = hasVideo;
    m_impl->owner = (__bridge NSView*)owner;
    if (hasVideo && m_impl->owner) {
        [m_impl->owner setWantsLayer:YES];
        m_impl->layer = [[AVPlayerLayer playerLayerWithPlayer:m_impl->player] retain];
        [m_impl->owner.layer addSublayer:m_impl->layer];
    }
    SetVolume(m_volume);
    SetMuted(m_muted);
    if (autoplay) Play();
    return true;
}

void PlatformMediaPlayer::SetRect(float x, float y, float w, float h) {
    if (!m_impl->layer || !m_impl->owner) return;
    CGFloat ownerH = m_impl->owner.bounds.size.height;
    m_impl->layer.frame = CGRectMake(x, ownerH - y - h, std::max(1.f, w), std::max(1.f, h));
}

void PlatformMediaPlayer::Play() { [m_impl->player play]; }
void PlatformMediaPlayer::Pause() { [m_impl->player pause]; }
void PlatformMediaPlayer::Stop() { [m_impl->player pause]; SetCurrentTime(0); }
void PlatformMediaPlayer::SetCurrentTime(double seconds) { if (m_impl->player) [m_impl->player seekToTime:CMTimeMakeWithSeconds(std::max(0.0, seconds), 600)]; }
double PlatformMediaPlayer::CurrentTime() const { return m_impl->player ? CMTimeGetSeconds(m_impl->player.currentTime) : 0.0; }
double PlatformMediaPlayer::Duration() const { return m_impl->player && m_impl->player.currentItem ? CMTimeGetSeconds(m_impl->player.currentItem.duration) : 0.0; }
void PlatformMediaPlayer::SetVolume(double volume) { m_volume = std::max(0.0, std::min(1.0, volume)); if (m_impl->player) m_impl->player.volume = (float)m_volume; }
double PlatformMediaPlayer::Volume() const { return m_volume; }
void PlatformMediaPlayer::SetMuted(bool muted) { m_muted = muted; if (m_impl->player) m_impl->player.muted = muted; }
bool PlatformMediaPlayer::Muted() const { return m_muted; }
bool PlatformMediaPlayer::Paused() const { return !m_impl->player || m_impl->player.rate == 0.0f; }

#endif
