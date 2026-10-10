#include <windows.h>
#include <libswscale/version_major.h>

// A correctly named DLL with either an incompatible ABI or missing symbols.
extern "C" __declspec(dllexport) unsigned swscale_version() {
    return (LIBSWSCALE_VERSION_MAJOR +
            (GetEnvironmentVariableW(L"AVB_TEST_WRONG_ABI", nullptr, 0) ? 1u : 0u)) << 16;
}
