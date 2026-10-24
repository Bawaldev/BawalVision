#include "bawal/backend/byte_range.hpp"

#include <algorithm>
#include <exception>
#include <string>

namespace bawal::backend {

std::optional<ByteRange> parseByteRange(std::string_view header, uint64_t size) {
    if (size == 0 || header.rfind("bytes=", 0) != 0) {
        return std::nullopt;
    }

    const std::string spec(header.substr(6));
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return std::nullopt;
    }

    try {
        const std::string first = spec.substr(0, dash);
        const std::string last = spec.substr(dash + 1);
        uint64_t start = 0;
        uint64_t end = 0;

        if (first.empty()) {
            const uint64_t suffix = std::stoull(last);
            if (suffix == 0) {
                return std::nullopt;
            }
            start = suffix >= size ? 0 : size - suffix;
            end = size - 1;
        } else {
            start = std::stoull(first);
            end = last.empty() ? size - 1 : std::stoull(last);
        }

        if (start >= size) {
            return std::nullopt;
        }
        end = std::min(end, size - 1);
        if (end < start) {
            return std::nullopt;
        }
        return ByteRange{start, end};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace bawal::backend
