#include "md5.h"
#ifdef FALCO_MODERN_BGFX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    void requireHashSuccess(NTSTATUS status)
    {
        if (status < 0)
            throw std::runtime_error("Windows MD5 provider failed (NTSTATUS " + std::to_string(status) + ")");
    }
    struct HashProvider
    {
        BCRYPT_ALG_HANDLE handle = nullptr;
        HashProvider() { requireHashSuccess(BCryptOpenAlgorithmProvider(&handle, BCRYPT_MD5_ALGORITHM, nullptr, 0)); }
        ~HashProvider() { BCryptCloseAlgorithmProvider(handle, 0); }
        HashProvider(const HashProvider&) = delete;
        HashProvider& operator=(const HashProvider&) = delete;
    };
    struct HashState
    {
        BCRYPT_HASH_HANDLE handle = nullptr;
        explicit HashState(BCRYPT_ALG_HANDLE provider)
        {
            requireHashSuccess(BCryptCreateHash(provider, &handle, nullptr, 0, nullptr, 0, 0));
        }
        ~HashState() { BCryptDestroyHash(handle); }
        HashState(const HashState&) = delete;
        HashState& operator=(const HashState&) = delete;
    };
}

std::string md5(const std::string text)
{
    static const HashProvider provider;
    HashState hash(provider.handle);
    size_t offset = 0;
    while (offset < text.size())
    {
        const ULONG count = static_cast<ULONG>(std::min(text.size() - offset,
            static_cast<size_t>(std::numeric_limits<ULONG>::max())));
        requireHashSuccess(BCryptHashData(hash.handle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(text.data() + offset)), count, 0));
        offset += count;
    }
    unsigned char digest[16];
    requireHashSuccess(BCryptFinishHash(hash.handle, digest, sizeof(digest), 0));
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (size_t i = 0; i < sizeof(digest); ++i)
    {
        result[2 * i] = digits[digest[i] >> 4];
        result[2 * i + 1] = digits[digest[i] & 15];
    }
    return result;
}
#endif
