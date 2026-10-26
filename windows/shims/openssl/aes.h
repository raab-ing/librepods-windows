// Minimal OpenSSL-compatible AES API implemented over Windows CNG (bcrypt).
// bleutils.cpp only uses AES_set_encrypt_key, AES_set_decrypt_key,
// AES_encrypt (single-block ECB) and AES_cbc_encrypt (single-block CBC);
// the shim covers exactly that surface so no OpenSSL dependency is needed.
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

struct AES_KEY
{
    unsigned char key[32];
    unsigned int keyBits;
};

#define AES_ENCRYPT 1
#define AES_DECRYPT 0

inline BCRYPT_ALG_HANDLE aesProvider(LPCWSTR chainingMode)
{
    static BCRYPT_ALG_HANDLE ecbProvider = [] {
        BCRYPT_ALG_HANDLE provider = nullptr;
        if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
        {
            BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_ECB)),
                              static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0);
        }
        return provider;
    }();
    static BCRYPT_ALG_HANDLE cbcProvider = [] {
        BCRYPT_ALG_HANDLE provider = nullptr;
        if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
        {
            BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_CBC)),
                              static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0);
        }
        return provider;
    }();
    return wcscmp(chainingMode, BCRYPT_CHAIN_MODE_CBC) == 0 ? cbcProvider : ecbProvider;
}

inline BCRYPT_KEY_HANDLE aesMakeKey(const AES_KEY *key, LPCWSTR chainingMode)
{
    BCRYPT_ALG_HANDLE provider = aesProvider(chainingMode);
    if (!provider)
    {
        return nullptr;
    }
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (BCryptGenerateSymmetricKey(provider, &keyHandle, nullptr, 0,
                                   const_cast<PUCHAR>(key->key),
                                   static_cast<ULONG>(key->keyBits / 8), 0) != 0)
    {
        return nullptr;
    }
    return keyHandle;
}

inline int AES_set_encrypt_key(const unsigned char *userKey, int bits, AES_KEY *key)
{
    if (!key || !userKey || (bits != 128 && bits != 192 && bits != 256))
    {
        return -1;
    }
    memcpy(key->key, userKey, static_cast<size_t>(bits / 8));
    key->keyBits = static_cast<unsigned int>(bits);
    return 0;
}

inline int AES_set_decrypt_key(const unsigned char *userKey, int bits, AES_KEY *key)
{
    // CNG uses the same key material for encrypt and decrypt.
    return AES_set_encrypt_key(userKey, bits, key);
}

inline void AES_encrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key)
{
    BCRYPT_KEY_HANDLE keyHandle = aesMakeKey(key, BCRYPT_CHAIN_MODE_ECB);
    if (!keyHandle)
    {
        return;
    }
    ULONG resultLength = 0;
    BCryptEncrypt(keyHandle, const_cast<PUCHAR>(in), 16, nullptr, nullptr, 0,
                  out, 16, &resultLength, 0);
    BCryptDestroyKey(keyHandle);
}

inline void AES_cbc_encrypt(const unsigned char *in, unsigned char *out, size_t length,
                            const AES_KEY *key, unsigned char *ivec, const int enc)
{
    BCRYPT_KEY_HANDLE keyHandle = aesMakeKey(key, BCRYPT_CHAIN_MODE_CBC);
    if (!keyHandle)
    {
        return;
    }
    unsigned char iv[16];
    memcpy(iv, ivec, 16);
    ULONG resultLength = 0;
    if (enc == AES_ENCRYPT)
    {
        BCryptEncrypt(keyHandle, const_cast<PUCHAR>(in), static_cast<ULONG>(length),
                      nullptr, iv, 16, out, static_cast<ULONG>(length), &resultLength, 0);
        if (length >= 16)
        {
            memcpy(ivec, out + length - 16, 16);
        }
    }
    else
    {
        BCryptDecrypt(keyHandle, const_cast<PUCHAR>(in), static_cast<ULONG>(length),
                      nullptr, iv, 16, out, static_cast<ULONG>(length), &resultLength, 0);
        if (length >= 16)
        {
            memcpy(ivec, in + length - 16, 16);
        }
    }
    BCryptDestroyKey(keyHandle);
}
