#include "base/sha256.h"

#include <algorithm>
#include <cstring>

namespace gt3r {

namespace {

constexpr u32 kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; ++i) w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(ByteSpan data) {
    total_ += data.size();
    size_t i = 0;
    if (len_) {
        size_t n = std::min(data.size(), 64 - len_);
        std::memcpy(buf_.data() + len_, data.data(), n);
        len_ += n, i = n;
        if (len_ < 64) return;
        block(buf_.data());
        len_ = 0;
    }
    for (; i + 64 <= data.size(); i += 64) block(data.data() + i);
    if (i < data.size()) std::memcpy(buf_.data(), data.data() + i, len_ = data.size() - i);
}

std::array<u8, 32> Sha256::finish() {
    u64 bits = total_ * 8;
    u8 pad = 0x80;
    update(ByteSpan(&pad, 1));
    u8 zero = 0;
    while (len_ != 56) update(ByteSpan(&zero, 1));
    u8 lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = u8(bits >> (56 - 8 * i));
    update(ByteSpan(lenb, 8));
    std::array<u8, 32> out;
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) out[size_t(4 * i + k)] = u8(h_[size_t(i)] >> (24 - 8 * k));
    return out;
}

std::array<u8, 32> sha256(ByteSpan data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

std::string sha256_hex(ByteSpan data) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (u8 b : sha256(data)) out += kHex[b >> 4], out += kHex[b & 15];
    return out;
}

std::array<u8, 32> hmac_sha256(ByteSpan key, ByteSpan message) {
    std::array<u8, 64> k{};
    if (key.size() > k.size()) {
        const std::array<u8, 32> h = sha256(key);
        std::copy(h.begin(), h.end(), k.begin());
    } else {
        std::copy(key.begin(), key.end(), k.begin());
    }
    std::array<u8, 64> pad;
    Sha256 inner;
    for (size_t i = 0; i < pad.size(); ++i) pad[i] = u8(k[i] ^ 0x36);
    inner.update(pad);
    inner.update(message);
    const std::array<u8, 32> ih = inner.finish();
    Sha256 outer;
    for (size_t i = 0; i < pad.size(); ++i) pad[i] = u8(k[i] ^ 0x5c);
    outer.update(pad);
    outer.update(ih);
    return outer.finish();
}

Bytes pbkdf2_sha256(ByteSpan password, ByteSpan salt, int iterations, size_t length) {
    Bytes out;
    for (u32 block = 1; out.size() < length; ++block) {
        Bytes s(salt.begin(), salt.end());
        for (int i = 3; i >= 0; --i) s.push_back(u8(block >> (8 * i)));
        std::array<u8, 32> u = hmac_sha256(password, s), t = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmac_sha256(password, u);
            for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

} // namespace gt3r
