#pragma once

// SHA-256 (FIPS 180-4), for content addressing (mod car packages, docs/dev/ONLINE.md), and
// HMAC-SHA256 (RFC 2104) / PBKDF2-HMAC-SHA256 (RFC 8018) for the network wheel's passwords
// (input/netxwheel.h).

#include "base/bytes.h"

#include <array>
#include <string>

namespace gt3r {

class Sha256 {
public:
    Sha256();
    void update(ByteSpan data);
    std::array<u8, 32> finish();

private:
    void block(const u8* p);
    std::array<u32, 8> h_;
    std::array<u8, 64> buf_{};
    size_t len_ = 0; // bytes in buf_
    u64 total_ = 0;  // bytes hashed
};

std::array<u8, 32> sha256(ByteSpan data);
std::string sha256_hex(ByteSpan data); // lower-case hex

std::array<u8, 32> hmac_sha256(ByteSpan key, ByteSpan message);
Bytes pbkdf2_sha256(ByteSpan password, ByteSpan salt, int iterations, size_t length);

} // namespace gt3r
