#include "audio.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

namespace {

// Volume change callback: runs on an audio thread, just pokes the UI thread.
class VolumeCallback : public IAudioEndpointVolumeCallback {
public:
    explicit VolumeCallback(HWND h) : hwnd_(h) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        PostMessageW(hwnd_, WM_APP_AUDIO, 0, 0);
        return S_OK;
    }

private:
    HWND hwnd_;
    LONG ref_ = 1;
};

class DeviceCallback : public IMMNotificationClient {
public:
    explicit DeviceCallback(HWND h) : hwnd_(h) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) PostMessageW(hwnd_, WM_APP_AUDIO_DEVICE, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        PostMessageW(hwnd_, WM_APP_AUDIO_DEVICE, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HWND hwnd_;
    LONG ref_ = 1;
};

}  // namespace

bool Audio::Init(HWND notifyWindow) {
    hwnd_ = notifyWindow;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enumerator_)))
        return false;
    auto* dc = new DeviceCallback(hwnd_);
    if (SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(dc))) deviceCallback_ = dc;
    else dc->Release();
    Attach();
    return true;
}

void Audio::Attach() {
    present = false;
    deviceName.clear();
    if (!enumerator_) return;
    IMMDevice* dev = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) return;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) deviceName = pv.pwszVal;
        PropVariantClear(&pv);
        props->Release();
    }
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&volume_))) {
        auto* cb = new VolumeCallback(hwnd_);
        if (SUCCEEDED(volume_->RegisterControlChangeNotify(cb))) volumeCallback_ = cb;
        else cb->Release();
        present = true;
        Refresh();
    }
    dev->Release();
}

void Audio::Detach() {
    if (volume_) {
        if (volumeCallback_) {
            volume_->UnregisterControlChangeNotify((IAudioEndpointVolumeCallback*)volumeCallback_);
            ((IAudioEndpointVolumeCallback*)volumeCallback_)->Release();
            volumeCallback_ = nullptr;
        }
        volume_->Release();
        volume_ = nullptr;
    }
    present = false;
}

void Audio::Reinit() {
    Detach();
    Attach();
}

void Audio::Refresh() {
    if (!volume_) return;
    float l = 0;
    BOOL m = FALSE;
    if (SUCCEEDED(volume_->GetMasterVolumeLevelScalar(&l))) level = l;
    if (SUCCEEDED(volume_->GetMute(&m))) muted = m != FALSE;
}

void Audio::SetLevel(float l) {
    if (!volume_) return;
    l = Clamp01(l);
    volume_->SetMasterVolumeLevelScalar(l, nullptr);
    if (muted && l > 0) volume_->SetMute(FALSE, nullptr);
    Refresh();
}

void Audio::ToggleMute() {
    if (!volume_) return;
    volume_->SetMute(muted ? FALSE : TRUE, nullptr);
    Refresh();
}

void Audio::Shutdown() {
    Detach();
    if (enumerator_) {
        if (deviceCallback_) {
            enumerator_->UnregisterEndpointNotificationCallback((IMMNotificationClient*)deviceCallback_);
            ((IMMNotificationClient*)deviceCallback_)->Release();
            deviceCallback_ = nullptr;
        }
        enumerator_->Release();
        enumerator_ = nullptr;
    }
}
