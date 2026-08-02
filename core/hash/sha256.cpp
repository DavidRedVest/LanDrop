#include "sha256.h"

#include <cstring>

namespace core {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256::Sha256() : m_totalLen(0), m_bufferLen(0) {
    m_state[0] = 0x6a09e667;
    m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372;
    m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f;
    m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab;
    m_state[7] = 0x5be0cd19;
    std::memset(m_buffer, 0, sizeof(m_buffer));
}

void Sha256::processBlock(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    m_totalLen += len;

    if (m_bufferLen > 0) {
        const size_t need = 64 - m_bufferLen;
        const size_t take = len < need ? len : need;
        std::memcpy(m_buffer + m_bufferLen, p, take);
        m_bufferLen += take;
        p += take;
        len -= take;
        if (m_bufferLen == 64) {
            processBlock(m_buffer);
            m_bufferLen = 0;
        }
    }

    while (len >= 64) {
        processBlock(p);
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(m_buffer, p, len);
        m_bufferLen = len;
    }
}

std::array<uint8_t, 32> Sha256::finalize() {
    const uint64_t bitLen = m_totalLen * 8; // 必须在追加填充字节之前算好原始长度

    const uint8_t pad = 0x80;
    update(&pad, 1);

    const uint8_t zero = 0x00;
    while (m_bufferLen != 56) {
        update(&zero, 1);
    }

    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
    }
    update(lenBytes, 8);

    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(m_state[i]);
    }
    return digest;
}

std::array<uint8_t, 32> Sha256::hash(const void* data, size_t len) {
    Sha256 sha;
    sha.update(data, len);
    return sha.finalize();
}

std::string Sha256::toHex(const std::array<uint8_t, 32>& digest) {
    static const char kHexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : digest) {
        result.push_back(kHexChars[(byte >> 4) & 0xF]);
        result.push_back(kHexChars[byte & 0xF]);
    }
    return result;
}

} // namespace core
