// SPDX-License-Identifier: Apache-2.0
#include "io/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace yc::io {

namespace {

constexpr char kMagic[4] = {'Y', 'C', 'G', 'E'};
constexpr size_t kHeader = 4 + 2 + 2 + 4 + 16 + 12 + 8;  // 48
constexpr size_t kTag = 16;
constexpr uint32_t kIterations = 200000;

template <class T>
void Put(std::string* s, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) s->push_back(static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF));
}
template <class T>
T Get(std::string_view s, size_t at) {
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[at + i])) << (8 * i);
  return static_cast<T>(v);
}

bool DeriveKey(const std::string& password, const uint8_t* salt, uint32_t iterations, uint8_t key[32]) {
  BCRYPT_ALG_HANDLE h = nullptr;
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return false;
  const NTSTATUS st = ::BCryptDeriveKeyPBKDF2(h, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
                                              const_cast<PUCHAR>(salt), 16, iterations, key, 32, 0);
  ::BCryptCloseAlgorithmProvider(h, 0);
  return BCRYPT_SUCCESS(st);
}

// AES-256-GCM in place; encrypt or decrypt (decrypt checks the tag)
bool Gcm(bool encrypt, const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len, uint8_t* data, size_t len, uint8_t tag[16]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE k = nullptr;
  bool ok = BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(::BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
            BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key), 32, 0));
  if (ok) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = 12;
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aad_len);
    info.pbTag = tag;
    info.cbTag = kTag;
    ULONG out = 0;
    const ULONG n = static_cast<ULONG>(len);
    ok = encrypt ? BCRYPT_SUCCESS(::BCryptEncrypt(k, data, n, &info, nullptr, 0, data, n, &out, 0))
                 : BCRYPT_SUCCESS(::BCryptDecrypt(k, data, n, &info, nullptr, 0, data, n, &out, 0));
  }
  if (k) ::BCryptDestroyKey(k);
  if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

}  // namespace

bool IsEncryptedYcg(std::string_view bytes) { return bytes.size() >= kHeader + kTag && std::memcmp(bytes.data(), kMagic, 4) == 0; }

bool EncryptYcg(std::string_view plain, const std::string& password, std::string* out) {
  if (plain.size() > 0xFFFFFFF0u) return false;  // one GCM call takes a ULONG length
  uint8_t salt[16], nonce[12], key[32], tag[16];
  if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, salt, sizeof salt, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
      !BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, nonce, sizeof nonce, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) || !DeriveKey(password, salt, kIterations, key)) {
    return false;
  }
  std::string s(kMagic, 4);
  Put<uint16_t>(&s, 1);
  Put<uint16_t>(&s, 1);
  Put<uint32_t>(&s, kIterations);
  s.append(reinterpret_cast<const char*>(salt), 16);
  s.append(reinterpret_cast<const char*>(nonce), 12);
  Put<uint64_t>(&s, plain.size());
  const size_t at = s.size();
  s.append(plain);
  if (!Gcm(true, key, nonce, reinterpret_cast<const uint8_t*>(s.data()), kHeader, reinterpret_cast<uint8_t*>(s.data() + at), plain.size(), tag)) return false;
  s.append(reinterpret_cast<const char*>(tag), kTag);
  SecureZeroMemory(key, sizeof key);
  *out = std::move(s);
  return true;
}

bool DecryptYcg(std::string_view enc, const std::string& password, std::string* plain) {
  if (!IsEncryptedYcg(enc)) return false;
  const uint16_t version = Get<uint16_t>(enc, 4), kdf = Get<uint16_t>(enc, 6);
  const uint32_t iterations = Get<uint32_t>(enc, 8);
  const uint64_t len = Get<uint64_t>(enc, 40);
  if (version != 1 || kdf != 1 || iterations == 0 || iterations > 10000000 || len != enc.size() - kHeader - kTag) return false;
  uint8_t salt[16], nonce[12], key[32], tag[16];
  std::memcpy(salt, enc.data() + 12, 16);
  std::memcpy(nonce, enc.data() + 28, 12);
  std::memcpy(tag, enc.data() + kHeader + len, kTag);
  if (!DeriveKey(password, salt, iterations, key)) return false;
  std::string out(enc.substr(kHeader, len));
  const bool ok = Gcm(false, key, nonce, reinterpret_cast<const uint8_t*>(enc.data()), kHeader, reinterpret_cast<uint8_t*>(out.data()), out.size(), tag);
  SecureZeroMemory(key, sizeof key);
  if (!ok) return false;
  *plain = std::move(out);
  return true;
}

}  // namespace yc::io
