#pragma once
#include "common.h"

struct IMMDeviceEnumerator;
struct IAudioEndpointVolume;

// Default playback device volume, event driven (no polling).
class Audio {
public:
    bool Init(HWND notifyWindow);
    void Shutdown();
    void Reinit();            // default device changed
    void Refresh();           // re-read level/mute after a change notification

    void SetLevel(float level);
    void ToggleMute();

    bool present = false;
    float level = 0;          // 0..1
    bool muted = false;
    std::wstring deviceName;

private:
    void Attach();
    void Detach();

    HWND hwnd_ = nullptr;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioEndpointVolume* volume_ = nullptr;
    void* volumeCallback_ = nullptr;
    void* deviceCallback_ = nullptr;
};
