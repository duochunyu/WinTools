#include "platform/win/hash.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>

namespace wintools::platform::win
{
namespace
{

constexpr wchar_t kHex[] = L"0123456789abcdef";

/// ntstatus.h redefines dozens of STATUS_* macros already present in winnt.h, which this
/// project builds with /WX. The success test itself is just the sign bit, so define it locally.
constexpr bool Ok(NTSTATUS status)
{
    return status >= 0;
}

struct Provider
{
    BCRYPT_ALG_HANDLE value{};
    ~Provider()
    {
        if (value != nullptr)
        {
            ::BCryptCloseAlgorithmProvider(value, 0);
        }
    }
    Provider() = default;
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
};

struct Hash
{
    BCRYPT_HASH_HANDLE value{};
    ~Hash()
    {
        if (value != nullptr)
        {
            ::BCryptDestroyHash(value);
        }
    }
    Hash() = default;
    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;
};

bool ToHex(const std::array<unsigned char, 32>& digest, std::string& hex)
{
    std::string result;
    result.reserve(64);
    for (const unsigned char byte : digest)
    {
        result.push_back(static_cast<char>(kHex[byte >> 4]));
        result.push_back(static_cast<char>(kHex[byte & 0x0F]));
    }
    hex = std::move(result);
    return true;
}

} // namespace

bool Sha256Bytes(std::string_view bytes, std::string& hex)
{
    Provider provider;
    if (!Ok(::BCryptOpenAlgorithmProvider(&provider.value, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0)))
    {
        return false;
    }

    Hash hash;
    if (!Ok(::BCryptCreateHash(provider.value, &hash.value, nullptr, 0, nullptr, 0, 0)))
    {
        return false;
    }
    if (!bytes.empty()
        && !Ok(::BCryptHashData(hash.value,
            reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), 0)))
    {
        return false;
    }

    std::array<unsigned char, 32> digest{};
    if (!Ok(::BCryptFinishHash(hash.value, digest.data(),
            static_cast<ULONG>(digest.size()), 0)))
    {
        return false;
    }
    return ToHex(digest, hex);
}

bool Sha256File(const std::filesystem::path& file, std::string& hex)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        return false;
    }

    Provider provider;
    if (!Ok(::BCryptOpenAlgorithmProvider(&provider.value, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0)))
    {
        return false;
    }

    Hash hash;
    if (!Ok(::BCryptCreateHash(provider.value, &hash.value, nullptr, 0, nullptr, 0, 0)))
    {
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0)
        {
            break;
        }
        if (!Ok(::BCryptHashData(hash.value,
                reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0)))
        {
            return false;
        }
    }
    if (input.bad())
    {
        return false;
    }

    std::array<unsigned char, 32> digest{};
    if (!Ok(::BCryptFinishHash(hash.value, digest.data(),
            static_cast<ULONG>(digest.size()), 0)))
    {
        return false;
    }
    return ToHex(digest, hex);
}

} // namespace wintools::platform::win
