#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>

inline uint32_t mf_fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[3])) << 24);
}

inline GUID mf_video_subtype_from_fourcc(uint32_t fourcc) {
    return GUID{fourcc, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

inline uint16_t mf_read_le16(const unsigned char *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t mf_read_le32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t mf_read_le64(const unsigned char *p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

inline void mf_write_le16(unsigned char *p, uint16_t value) {
    p[0] = static_cast<unsigned char>(value & 0xff);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xff);
}

inline void mf_write_le32(unsigned char *p, uint32_t value) {
    p[0] = static_cast<unsigned char>(value & 0xff);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xff);
    p[2] = static_cast<unsigned char>((value >> 16) & 0xff);
    p[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

inline void mf_write_le64(unsigned char *p, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xff);
}

class MfStartupScope {
public:
    explicit MfStartupScope(ULONG flags = MFSTARTUP_LITE);
    ~MfStartupScope();

    MfStartupScope(const MfStartupScope &) = delete;
    MfStartupScope &operator=(const MfStartupScope &) = delete;

    bool started() const { return m_started; }

private:
    bool m_started = false;
};

HRESULT mf_create_d3d11_device_manager(
    ID3D11Device *input_device,
    ID3D11Device **device,
    IMFDXGIDeviceManager **manager);

HRESULT mf_get_event_with_timeout(
    IMFMediaEventGenerator *events,
    DWORD timeout_ms,
    IMFMediaEvent **out);

HRESULT mf_create_transform(
    const GUID &category,
    const MFT_REGISTER_TYPE_INFO *input_type,
    const MFT_REGISTER_TYPE_INFO *output_type,
    IMFTransform **out,
    bool *is_async);

#endif
