#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "avb_mediafoundation_network.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <propsys.h>
#include <windows.h>
#include <wrl/client.h>

#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

std::string to_utf8(const std::wstring &w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n,
                            nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                      nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX as the byte it stands for; the bytes read as UTF-8.
std::wstring percent_decoded(const std::wstring &w) {
    const std::string s = to_utf8(w);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return from_utf8(out);
}

// Hands the source the user and password it was given when the server asks;
// once the server has refused them twice, refuses the source in turn, so a
// wrong password ends the open rather than asking for ever.
class CredentialManager final : public IMFNetCredentialManager {
public:
    CredentialManager(std::wstring user, std::wstring password)
        : m_user(std::move(user)), m_password(std::move(password)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IMFNetCredentialManager)) {
            *ppv = static_cast<IMFNetCredentialManager *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refs);
    }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) delete this;
        return refs;
    }

    STDMETHODIMP BeginGetCredentials(MFNetCredentialManagerGetParam *param,
                                     IMFAsyncCallback *callback,
                                     IUnknown *state) override {
        if (!param || !callback) return E_POINTER;
        HRESULT status = S_OK;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Asked again and again: the server refuses what was given.
            if (m_refused >= 2 || param->nRetries > 2) {
                status = E_ACCESSDENIED;
            } else {
                if (!m_cache) status = MFCreateCredentialCache(&m_cache);
                ComPtr<IMFNetCredential> credential;
                DWORD requirements = 0;
                // Basic sends the password as it is: the cache is told so.
                const DWORD flags =
                    param->fClearTextPackage ? MFNET_AUTHENTICATION_CLEAR_TEXT
                                             : 0;
                if (SUCCEEDED(status))
                    status = m_cache->GetCredential(
                        param->pszUrl, param->pszRealm, flags, &credential,
                        &requirements);
                // Wide strings, their terminators counted: as the network
                // source reads them when the data is not encrypted.
                if (SUCCEEDED(status))
                    status = credential->SetUser(
                        reinterpret_cast<BYTE *>(m_user.data()),
                        (DWORD)((m_user.size() + 1) * sizeof(wchar_t)), FALSE);
                if (SUCCEEDED(status))
                    status = credential->SetPassword(
                        reinterpret_cast<BYTE *>(m_password.data()),
                        (DWORD)((m_password.size() + 1) * sizeof(wchar_t)),
                        FALSE);
                if (SUCCEEDED(status)) m_credential = credential;
            }
        }
        ComPtr<IMFAsyncResult> result;
        HRESULT hr = MFCreateAsyncResult(nullptr, callback, state, &result);
        if (FAILED(hr)) return hr;
        result->SetStatus(status);
        return MFInvokeCallback(result.Get());
    }

    STDMETHODIMP EndGetCredentials(IMFAsyncResult *result,
                                   IMFNetCredential **credential) override {
        if (!result || !credential) return E_POINTER;
        *credential = nullptr;
        const HRESULT status = result->GetStatus();
        if (FAILED(status)) return status;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_credential) return E_UNEXPECTED;
        *credential = m_credential.Get();
        (*credential)->AddRef();
        return S_OK;
    }

    STDMETHODIMP SetGood(IMFNetCredential *, BOOL good) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (good)
            m_refused = 0;
        else
            ++m_refused;
        return S_OK;
    }

private:
    ~CredentialManager() = default;

    volatile LONG m_refs = 1;
    std::mutex m_mutex;
    std::wstring m_user, m_password;
    ComPtr<IMFNetCredentialCache> m_cache;
    ComPtr<IMFNetCredential> m_credential;
    int m_refused = 0;
};

} // namespace

MfUrlCredentials mf_split_url_credentials(const std::wstring &url) {
    MfUrlCredentials out;
    out.url = url;
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos || scheme == 0) return out;
    const size_t start = scheme + 3;
    size_t end = url.find_first_of(L"/?#", start);
    if (end == std::wstring::npos) end = url.size();
    const size_t at = url.rfind(L'@', end == 0 ? 0 : end - 1);
    if (at == std::wstring::npos || at < start) return out;
    const std::wstring info = url.substr(start, at - start);
    const size_t colon = info.find(L':');
    out.user = percent_decoded(info.substr(0, colon));
    if (colon != std::wstring::npos)
        out.password = percent_decoded(info.substr(colon + 1));
    out.url = url.substr(0, start) + url.substr(at + 1);
    out.present = !out.user.empty();
    return out;
}

HRESULT mf_create_network_source(const std::wstring &url,
                                 const std::wstring &user,
                                 const std::wstring &password,
                                 IMFMediaSource **source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    ComPtr<IPropertyStore> props;
    HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&props));
    if (FAILED(hr)) return hr;

    ComPtr<IMFNetCredentialManager> manager;
    manager.Attach(new CredentialManager(user, password));
    // The network source's properties are keyed by their GUID, pid 0.
    PROPERTYKEY key{MFNETSOURCE_CREDENTIAL_MANAGER, 0};
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_UNKNOWN;
    value.punkVal = manager.Get(); // the store keeps its own reference
    hr = props->SetValue(key, value);
    if (FAILED(hr)) return hr;

    ComPtr<IMFSourceResolver> resolver;
    hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) return hr;
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> object;
    hr = resolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                       props.Get(), &type, &object);
    if (FAILED(hr)) return hr;
    return object.CopyTo(source);
}
