#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::server {

inline uint32_t sha256RotateRight(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

inline std::string sha256Hex(std::string_view input) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };

    std::array<uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    std::vector<uint8_t> message(input.begin(), input.end());
    const uint64_t bit_length = static_cast<uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0x00U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xffU));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        std::array<uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t base = offset + i * 4U;
            w[i] = (static_cast<uint32_t>(message[base]) << 24U) |
                   (static_cast<uint32_t>(message[base + 1]) << 16U) |
                   (static_cast<uint32_t>(message[base + 2]) << 8U) |
                   static_cast<uint32_t>(message[base + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = sha256RotateRight(w[i - 15], 7U) ^
                                sha256RotateRight(w[i - 15], 18U) ^
                                (w[i - 15] >> 3U);
            const uint32_t s1 = sha256RotateRight(w[i - 2], 17U) ^
                                sha256RotateRight(w[i - 2], 19U) ^
                                (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = sha256RotateRight(e, 6U) ^
                                sha256RotateRight(e, 11U) ^
                                sha256RotateRight(e, 25U);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = sha256RotateRight(a, 2U) ^
                                sha256RotateRight(a, 13U) ^
                                sha256RotateRight(a, 22U);
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

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            output.push_back(kHex[(word >> shift) & 0x0fU]);
        }
    }
    return output;
}

} // namespace bawal::server
