#pragma once

#ifdef _WIN32

#include <mfidl.h>
#include <string>

// Network sources that ask for a user: an IP camera's rtsp://, a server's
// http:// behind authentication.
//
// Media Foundation's network source does not read a user and password from
// the URL. It sends the first request without them and, answered 401, asks a
// credential manager -- with none it gives up. So a URL's credentials are
// taken out of it and handed to the source through MFNETSOURCE_CREDENTIAL_
// MANAGER, and the source answers the server's challenge itself (Basic,
// Digest, ...).

struct MfUrlCredentials {
    std::wstring url;      // without "user:password@"
    std::wstring user;     // percent-decoded
    std::wstring password; // percent-decoded
    bool present = false;  // whether the URL named a user
};

// "rtsp://user:pa%40ss@host/path" -> "rtsp://host/path", "user", "pa@ss". A
// URL without a user, or a file path, comes back as it is.
MfUrlCredentials mf_split_url_credentials(const std::wstring &url);

// A media source for `url`, answering the server's authentication as `user`
// with `password`. E_ACCESSDENIED once the server has refused them.
HRESULT mf_create_network_source(const std::wstring &url,
                                 const std::wstring &user,
                                 const std::wstring &password,
                                 IMFMediaSource **source);

#endif
