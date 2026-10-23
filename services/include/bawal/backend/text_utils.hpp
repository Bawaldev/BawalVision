#pragma once

#include <cctype>
#include <string>

namespace bawal::backend {

/** Trim leading/trailing ASCII whitespace from a copy of the input. */
[[nodiscard]] inline std::string trimCopy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

/** Lowercase ASCII characters in place-safe copy form. */
[[nodiscard]] inline std::string toLowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

} // namespace bawal::backend
