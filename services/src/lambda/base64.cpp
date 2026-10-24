#include "bawal/lambda/base64.hpp"

#include <algorithm>
#include <limits>

#include <openssl/evp.h>

namespace bawal::lambda {

std::vector<uint8_t> decodeBase64(std::string_view encoded) {
    std::string compact;
    compact.reserve(encoded.size());
    for (const char ch : encoded) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        compact.push_back(ch);
    }

    if (compact.empty() || compact.size() % 4 != 0 ||
        compact.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    std::vector<uint8_t> decoded((compact.size() / 4) * 3);
    const int written = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(compact.data()),
        static_cast<int>(compact.size()));
    if (written < 0) {
        return {};
    }

    std::size_t padding = 0;
    if (compact.size() >= 1 && compact.back() == '=') {
        ++padding;
    }
    if (compact.size() >= 2 && compact[compact.size() - 2] == '=') {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(written) - padding);
    return decoded;
}

std::string encodeBase64(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const std::size_t encoded_size = 4 * ((size + 2) / 3);
    std::string encoded(encoded_size, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), data,
        static_cast<int>(size));
    if (written < 0) {
        return {};
    }
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

} // namespace bawal::lambda
