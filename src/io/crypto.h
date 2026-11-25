// SPDX-License-Identifier: Apache-2.0
// crypto.h - password-protected workbooks ("Encrypt with Password")
//
// An encrypted .ycg is an envelope around the ordinary file bytes (little-endian):
//   magic "YCGE" | u16 version (1) | u16 kdf (1 = PBKDF2-HMAC-SHA256) | u32 iterations | 16-byte salt | 12-byte nonce
//   | u64 plain length | ciphertext (AES-256-GCM) | 16-byte tag
// The 48-byte header is authenticated as additional data. Normative spec: docs/ycg-format.md.
#pragma once

#include <string>
#include <string_view>

namespace yc::io {

bool IsEncryptedYcg(std::string_view bytes);
// false when encryption isn't available (the Windows CNG provider failed)
bool EncryptYcg(std::string_view plain, const std::string& password, std::string* out);
// false: wrong password or a damaged file
bool DecryptYcg(std::string_view enc, const std::string& password, std::string* plain);

}  // namespace yc::io
