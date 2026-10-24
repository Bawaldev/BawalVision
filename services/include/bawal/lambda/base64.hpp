#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::lambda {

/**
 * @brief Decode a standard (optionally padded) base64 string.
 *
 * Whitespace is ignored. Returns an empty vector for malformed input.
 */
[[nodiscard]] std::vector<uint8_t> decodeBase64(std::string_view encoded);

/**
 * @brief Encode arbitrary bytes as standard padded base64.
 */
[[nodiscard]] std::string encodeBase64(const uint8_t* data, std::size_t size);

[[nodiscard]] inline std::string encodeBase64(const std::vector<uint8_t>& data) {
    return encodeBase64(data.data(), data.size());
}

} // namespace bawal::lambda
