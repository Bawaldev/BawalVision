#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bawal::backend {

/** Inclusive byte range parsed from an HTTP `Range: bytes=...` header. */
struct alignas(16) ByteRange {
    uint64_t start{0};
    uint64_t end{0};
};

static_assert(sizeof(ByteRange) == 16, "ByteRange must stay exactly 16 bytes");
static_assert(alignof(ByteRange) == 16, "ByteRange must stay 16-byte aligned");

/**
 * @brief Parse a single HTTP byte range for a resource of @p size bytes.
 *
 * Only the `bytes=start-end`, `bytes=start-`, and `bytes=-suffix` forms are
 * supported; multi-range requests return std::nullopt.
 */
[[nodiscard]] std::optional<ByteRange> parseByteRange(std::string_view header,
                                                      uint64_t size);

} // namespace bawal::backend
