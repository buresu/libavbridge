#include "avb_mediafoundation_common.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mfapi.h>
#include <mferror.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

MfStartupScope::MfStartupScope(ULONG flags) {
    m_started = SUCCEEDED(MFStartup(MF_VERSION, flags));
}

MfStartupScope::~MfStartupScope() {
    if (m_started) MFShutdown();
}

HRESULT mf_create_d3d11_device_manager(
    ID3D11Device *input_device,
    ID3D11Device **device,
    IMFDXGIDeviceManager **manager) {
    if (!device || !manager) return E_POINTER;
    *device = nullptr;
    *manager = nullptr;

    ComPtr<ID3D11Device> created_device = input_device;
    HRESULT hr = S_OK;
    if (!created_device) {
        D3D_FEATURE_LEVEL feature_level{};
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &created_device, &feature_level, nullptr);
        if (FAILED(hr)) return hr;
    }

    ComPtr<IMFDXGIDeviceManager> created_manager;
    UINT reset_token = 0;
    hr = MFCreateDXGIDeviceManager(&reset_token, &created_manager);
    if (SUCCEEDED(hr))
        hr = created_manager->ResetDevice(created_device.Get(), reset_token);
    if (FAILED(hr)) return hr;

    hr = created_device.CopyTo(device);
    if (SUCCEEDED(hr)) hr = created_manager.CopyTo(manager);
    return hr;
}

HRESULT mf_get_event_with_timeout(
    IMFMediaEventGenerator *events,
    DWORD timeout_ms,
    IMFMediaEvent **out) {
    if (!events || !out) return E_POINTER;
    *out = nullptr;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, out);
        if (hr != MF_E_NO_EVENTS_AVAILABLE) return hr;
        if (GetTickCount64() >= deadline)
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

HRESULT mf_create_transform(
    const GUID &category,
    const MFT_REGISTER_TYPE_INFO *input_type,
    const MFT_REGISTER_TYPE_INFO *output_type,
    IMFTransform **out,
    bool *is_async) {
    if (!out || !is_async) return E_POINTER;
    *out = nullptr;
    *is_async = false;

    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        category, MFT_ENUM_FLAG_ALL,
        input_type, output_type, &activates, &count);
    if (SUCCEEDED(hr) && count == 0) hr = MF_E_TOPO_CODEC_NOT_FOUND;
    if (SUCCEEDED(hr)) {
        hr = MF_E_TOPO_CODEC_NOT_FOUND;
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> candidate;
            HRESULT activate_hr = activates[i]->ActivateObject(
                IID_PPV_ARGS(&candidate));
            if (FAILED(activate_hr) || !candidate) {
                hr = activate_hr;
                continue;
            }

            ComPtr<IMFAttributes> attributes;
            UINT32 async_flag = FALSE;
            if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes)
                attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag);
            if (async_flag &&
                (!attributes ||
                 FAILED(attributes->SetUINT32(
                     MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))) {
                activates[i]->ShutdownObject();
                continue;
            }

            hr = candidate.CopyTo(out);
            *is_async = async_flag != FALSE;
            break;
        }
    }

    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return hr;
}

#endif
