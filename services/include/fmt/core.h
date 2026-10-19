#pragma once

#if defined(BAWAL_USE_SYSTEM_FMT) && __has_include_next(<fmt/core.h>)
#include_next <fmt/core.h>
#else

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fmt {

template <typename... Args>
using format_string = std::format_string<Args...>;

/**
 * @brief Formats arguments according to the format string using C++20 std::format.
 *
 * @param fmt_str Compile-time checked format specification string.
 * @param args Arguments to be formatted.
 * @return std::string Formatted output string.
 */
template <typename... Args>
[[nodiscard]] inline std::string format(format_string<Args...> fmt_str, Args&&... args) {
    return std::format(fmt_str, std::forward<Args>(args)...);
}

/**
 * @brief Prints formatted output to the specified C FILE stream (e.g., stdout or stderr).
 *
 * @param stream Destination FILE pointer.
 * @param fmt_str Compile-time checked format specification string.
 * @param args Arguments to be formatted.
 * @return void
 */
template <typename... Args>
inline void print(std::FILE* stream, format_string<Args...> fmt_str, Args&&... args) {
    const std::string out = std::format(fmt_str, std::forward<Args>(args)...);
    std::fwrite(out.data(), 1, out.size(), stream);
}

/**
 * @brief Prints formatted output directly to standard output (stdout).
 *
 * @param fmt_str Compile-time checked format specification string.
 * @param args Arguments to be formatted.
 * @return void
 */
template <typename... Args>
inline void print(format_string<Args...> fmt_str, Args&&... args) {
    print(stdout, fmt_str, std::forward<Args>(args)...);
}

} // namespace fmt

#endif

