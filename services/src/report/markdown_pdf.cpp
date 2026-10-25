#include "bawal/report/markdown_pdf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

namespace bawal::report {

namespace fs = std::filesystem;

namespace {

constexpr double kPageWidth = 595.28;
constexpr double kPageHeight = 841.89;
constexpr double kMarginLeft = 46.0;
constexpr double kMarginRight = 46.0;
constexpr double kMarginTop = 46.0;
constexpr double kMarginBottom = 48.0;
constexpr double kContentWidth = kPageWidth - kMarginLeft - kMarginRight;

struct Rgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};
};

constexpr Rgb kNavy{0.059, 0.090, 0.165};
constexpr Rgb kSlate{0.300, 0.350, 0.430};
constexpr Rgb kMuted{0.480, 0.520, 0.590};
constexpr Rgb kBlue{0.130, 0.330, 0.610};
constexpr Rgb kLightBlue{0.820, 0.880, 0.960};
constexpr Rgb kHeaderFill{0.915, 0.940, 0.975};
constexpr Rgb kAltFill{0.972, 0.978, 0.988};
constexpr Rgb kCodeFill{0.945, 0.955, 0.970};
constexpr Rgb kWhite{1.0, 1.0, 1.0};
constexpr Rgb kGreen{0.086, 0.639, 0.290};
constexpr Rgb kRed{0.863, 0.149, 0.149};
constexpr Rgb kAmber{0.851, 0.467, 0.024};

enum class Face : uint8_t { Regular, Bold, Italic, Mono };

struct Span {
    std::string text;
    Face face{Face::Regular};
};

struct Token {
    std::string text;
    Face face{Face::Regular};
    bool space{false};
};

struct EmbeddedImage {
    int width{0};
    int height{0};
    int components{3};
    std::vector<uint8_t> bytes;
};

std::string trim(std::string_view input) {
    std::size_t begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool isHorizontalRule(std::string_view line) {
    const std::string compact = [&]() {
        std::string out;
        for (char c : line) {
            if (c == ' ' || c == '\t') continue;
            out.push_back(c);
        }
        return out;
    }();
    if (compact.size() < 3) return false;
    const char first = compact.front();
    if (first != '-' && first != '*' && first != '_') return false;
    return std::all_of(compact.begin(), compact.end(),
                       [first](char c) { return c == first; });
}

int headingLevel(std::string_view line) {
    int level = 0;
    while (level < static_cast<int>(line.size()) && line[level] == '#') {
        ++level;
    }
    if (level == 0 || level > 6) return 0;
    if (level < static_cast<int>(line.size()) && line[level] != ' ' &&
        line[level] != '\t') {
        return 0;
    }
    return level;
}

void appendCodepoint(std::string& out, uint32_t cp) {
    switch (cp) {
        case 0x09: out.push_back(' '); break;
        case 0x2013:
        case 0x2014: out.push_back('-'); break;
        case 0x2018:
        case 0x2019: out.push_back('\''); break;
        case 0x201C:
        case 0x201D: out.push_back('"'); break;
        case 0x2022: out.push_back(static_cast<char>(0x95)); break;
        case 0x2026: out += "..."; break;
        case 0x2190: out += "<-"; break;
        case 0x2192: out += "->"; break;
        case 0x2264: out += "<="; break;
        case 0x2265: out += ">="; break;
        case 0x00B0:
        case 0x00B1:
        case 0x00B5:
        case 0x00D7:
        case 0x00F7:
            out.push_back(static_cast<char>(cp));
            break;
        case 0x2705: out += "[OK]"; break;
        case 0x274C:
        case 0x26D4: out += "[X]"; break;
        case 0x23F3: break;
        case 0x1F321: out += "Temp"; break;
        case 0x1F4C4: out += "Report"; break;
        case 0x1F5C2: out += "Evidence"; break;
        case 0xFE0F: break;
        default:
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp >= 0xA0 && cp <= 0xFF) {
                out.push_back(static_cast<char>(cp));
            } else {
                out.push_back('?');
            }
            break;
    }
}

std::string sanitizeUtf8(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            if (c != '\r' && c != '\0') {
                out.push_back(static_cast<char>(c));
            }
            ++i;
            continue;
        }

        uint32_t cp = 0;
        std::size_t length = 0;
        if ((c & 0xE0U) == 0xC0U) {
            cp = c & 0x1FU;
            length = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            cp = c & 0x0FU;
            length = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            cp = c & 0x07U;
            length = 4;
        } else {
            out.push_back('?');
            ++i;
            continue;
        }

        if (i + length > input.size()) {
            out.push_back('?');
            break;
        }
        bool valid = true;
        for (std::size_t k = 1; k < length; ++k) {
            const unsigned char continuation =
                static_cast<unsigned char>(input[i + k]);
            if ((continuation & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
            cp = (cp << 6U) | (continuation & 0x3FU);
        }
        if (!valid) {
            out.push_back('?');
            ++i;
            continue;
        }
        appendCodepoint(out, cp);
        i += length;
    }
    return out;
}

std::string pdfEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const unsigned char c : text) {
        switch (c) {
            case '(': out += "\\("; break;
            case ')': out += "\\)"; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c < 32 || c >= 127) {
                    out += fmt::format("\\{:03o}", static_cast<unsigned>(c));
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

const char* fontName(Face face) {
    switch (face) {
        case Face::Bold: return "F2";
        case Face::Italic: return "F3";
        case Face::Mono: return "F4";
        default: return "F1";
    }
}

double charWidth(unsigned char c) {
    if (c == ' ') return 0.28;
    if (std::strchr("ilI.,;:!|'`", c) != nullptr) return 0.29;
    if (std::strchr("ftrj()[]{}-/\\\"", c) != nullptr) return 0.36;
    if (std::strchr("mwMW@%&", c) != nullptr) return 0.86;
    if (c >= 'A' && c <= 'Z') return 0.67;
    if (c >= '0' && c <= '9') return 0.56;
    if (c >= 0x80) return 0.56;
    return 0.51;
}

double measureText(std::string_view text, Face face, double size) {
    if (face == Face::Mono) {
        return static_cast<double>(text.size()) * size * 0.60;
    }
    double width = 0.0;
    for (const unsigned char c : text) {
        width += charWidth(c);
    }
    if (face == Face::Bold) width *= 1.06;
    return width * size;
}

std::vector<Span> parseInline(std::string_view raw, Face base = Face::Regular) {
    const std::string text = sanitizeUtf8(raw);
    std::vector<Span> spans;
    std::string buffer;

    bool bold = base == Face::Bold;
    bool italic = base == Face::Italic;
    bool mono = base == Face::Mono;

    auto currentFace = [&]() -> Face {
        if (mono) return Face::Mono;
        if (bold) return Face::Bold;
        if (italic) return Face::Italic;
        return Face::Regular;
    };
    auto flush = [&]() {
        if (!buffer.empty()) {
            spans.push_back({buffer, currentFace()});
            buffer.clear();
        }
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            buffer.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
            flush();
            bold = !bold;
            i += 2;
            continue;
        }
        if (c == '_' && i + 1 < text.size() && text[i + 1] == '_') {
            flush();
            bold = !bold;
            i += 2;
            continue;
        }
        if (c == '`') {
            flush();
            mono = !mono;
            ++i;
            continue;
        }
        if (c == '*') {
            flush();
            italic = !italic;
            ++i;
            continue;
        }
        if (c == '_') {
            const bool previousWord =
                i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0;
            const bool nextWord =
                i + 1 < text.size() &&
                std::isalnum(static_cast<unsigned char>(text[i + 1])) != 0;
            if (!previousWord && nextWord) {
                flush();
                italic = true;
                ++i;
                continue;
            }
            if (previousWord && !nextWord && italic) {
                flush();
                italic = false;
                ++i;
                continue;
            }
        }
        if (c == '[') {
            const std::size_t close = text.find(']', i + 1);
            if (close != std::string::npos && close + 1 < text.size() &&
                text[close + 1] == '(') {
                const std::size_t end = text.find(')', close + 2);
                if (end != std::string::npos) {
                    buffer += text.substr(i + 1, close - i - 1);
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == '<') {
            const std::size_t end = text.find('>', i + 1);
            if (end != std::string::npos) {
                i = end + 1;
                continue;
            }
        }
        buffer.push_back(c);
        ++i;
    }
    flush();
    if (spans.empty()) {
        spans.push_back({"", Face::Regular});
    }
    return spans;
}

std::string flattenSpans(const std::vector<Span>& spans) {
    std::string out;
    for (const auto& span : spans) {
        out += span.text;
    }
    return out;
}

std::vector<Token> tokenize(const std::vector<Span>& spans) {
    std::vector<Token> tokens;
    for (const auto& span : spans) {
        std::size_t i = 0;
        while (i < span.text.size()) {
            const bool space =
                std::isspace(static_cast<unsigned char>(span.text[i])) != 0;
            std::size_t end = i;
            while (end < span.text.size() &&
                   (std::isspace(static_cast<unsigned char>(span.text[end])) != 0) ==
                       space) {
                ++end;
            }
            tokens.push_back({span.text.substr(i, end - i), span.face, space});
            i = end;
        }
    }
    return tokens;
}

std::vector<std::string> wrapPlainText(std::string_view text, double max_width,
                                       Face face, double size) {
    std::vector<std::string> lines;
    const std::vector<Span> spans{{std::string(text), face}};
    const std::vector<Token> tokens = tokenize(spans);
    std::string current;
    double current_width = 0.0;
    bool pending_space = false;

    auto pushWord = [&](const std::string& word) {
        const double word_width = measureText(word, face, size);
        if (!current.empty() && current_width + word_width > max_width) {
            lines.push_back(current);
            current.clear();
            current_width = 0.0;
        }
        if (word_width <= max_width) {
            current += word;
            current_width += word_width;
            return;
        }
        std::string chunk;
        for (const char ch : word) {
            const std::string candidate = chunk + ch;
            if (!chunk.empty() &&
                measureText(candidate, face, size) > max_width) {
                if (!current.empty()) {
                    lines.push_back(current);
                    current.clear();
                    current_width = 0.0;
                }
                lines.push_back(chunk);
                chunk.assign(1, ch);
            } else {
                chunk = candidate;
            }
        }
        if (!chunk.empty()) {
            current += chunk;
            current_width += measureText(chunk, face, size);
        }
    };

    for (const auto& token : tokens) {
        if (token.space) {
            pending_space = !current.empty();
            continue;
        }
        if (pending_space) {
            const double space_width = measureText(" ", face, size);
            if (!current.empty() && current_width + space_width <= max_width) {
                current.push_back(' ');
                current_width += space_width;
            }
            pending_space = false;
        }
        pushWord(token.text);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

std::vector<std::string> wrapMono(std::string_view text, double max_width,
                                  double size) {
    const int max_chars = std::max(
        8, static_cast<int>(std::floor(max_width / (size * 0.60))));
    std::vector<std::string> lines;
    if (text.empty()) {
        lines.emplace_back();
        return lines;
    }
    for (std::size_t i = 0; i < text.size(); i += static_cast<std::size_t>(max_chars)) {
        lines.emplace_back(text.substr(i, static_cast<std::size_t>(max_chars)));
    }
    return lines;
}

std::vector<std::string> splitTableRow(std::string_view line) {
    std::string cleaned = trim(line);
    if (!cleaned.empty() && cleaned.front() == '|') {
        cleaned.erase(cleaned.begin());
    }
    if (!cleaned.empty() && cleaned.back() == '|') {
        cleaned.pop_back();
    }

    std::vector<std::string> cells;
    std::string current;
    bool escaped = false;
    for (const char c : cleaned) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            cells.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    cells.push_back(trim(current));
    if (cells.empty()) {
        cells.emplace_back();
    }
    return cells;
}

bool isTableSeparator(std::string_view line) {
    bool has_dash = false;
    for (const char c : line) {
        if (c == '-') {
            has_dash = true;
            continue;
        }
        if (c == '|' || c == ':' || c == ' ' || c == '\t') {
            continue;
        }
        return false;
    }
    return has_dash;
}

bool isTableStart(const std::vector<std::string>& lines, std::size_t index) {
    if (index + 1 >= lines.size()) return false;
    if (lines[index].find('|') == std::string::npos) return false;
    return isTableSeparator(lines[index + 1]);
}

std::vector<int> jpegDimensions(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return {};
    }
    std::size_t i = 2;
    while (i + 3 < bytes.size()) {
        if (bytes[i] != 0xFF) {
            ++i;
            continue;
        }
        const uint8_t marker = bytes[i + 1];
        i += 2;
        if (marker == 0xD8 || marker == 0xD9) continue;
        if (marker == 0xDA) break;
        if (i + 1 >= bytes.size()) break;
        const std::size_t length =
            (static_cast<std::size_t>(bytes[i]) << 8U) | bytes[i + 1];
        if (length < 2 || i + length > bytes.size()) break;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
            marker != 0xC8 && marker != 0xCC) {
            const std::size_t height =
                (static_cast<std::size_t>(bytes[i + 3]) << 8U) | bytes[i + 4];
            const std::size_t width =
                (static_cast<std::size_t>(bytes[i + 5]) << 8U) | bytes[i + 6];
            const int components = bytes[i + 7];
            if (width == 0 || height == 0) return {};
            return {static_cast<int>(width), static_cast<int>(height), components};
        }
        i += length;
    }
    return {};
}

std::vector<uint8_t> readBinaryFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) return {};
    return bytes;
}

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> decodeDataUriImage(std::string_view target) {
    if (!startsWith(target, "data:")) return {};
    const std::size_t comma = target.find(',');
    if (comma == std::string_view::npos) return {};
    const std::string_view header = target.substr(0, comma);
    if (header.find(";base64") == std::string_view::npos) return {};

    const std::string_view encoded = target.substr(comma + 1);
    std::vector<uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);
    uint32_t accumulator = 0;
    int bits = -8;
    for (const unsigned char c : encoded) {
        if (c == '=') break;
        const int value = base64Value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6U) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFU));
            bits -= 8;
        }
    }
    return out;
}

std::vector<fs::path> allowedImageRoots() {
    std::vector<fs::path> roots;
    if (const char* configured = std::getenv("BAWAL_IMAGE_DIR");
        configured != nullptr && *configured != '\0') {
        roots.emplace_back(configured);
    }
    roots.emplace_back("./data/inspections");
    roots.emplace_back("/app/data/inspections");
    roots.emplace_back("/tmp/bawal_batches");
    return roots;
}

bool isWithinRoot(const fs::path& candidate, const fs::path& root) {
    std::error_code ec;
    const fs::path canonical_candidate = fs::weakly_canonical(candidate, ec);
    if (ec) return false;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec) return false;

    auto candidate_it = canonical_candidate.begin();
    for (auto root_it = canonical_root.begin(); root_it != canonical_root.end();
         ++root_it, ++candidate_it) {
        if (candidate_it == canonical_candidate.end() ||
            *candidate_it != *root_it) {
            return false;
        }
    }
    return true;
}

bool isAllowedImagePath(const fs::path& candidate) {
    for (const auto& root : allowedImageRoots()) {
        if (isWithinRoot(candidate, root)) {
            return true;
        }
    }
    return false;
}

std::optional<fs::path> resolveImageTarget(std::string_view target) {
    if (target.empty() || startsWith(target, "data:")) {
        return std::nullopt;
    }

    std::string path_text(target);
    if (startsWith(path_text, "http://") || startsWith(path_text, "https://")) {
        const std::size_t scheme = path_text.find("://");
        const std::size_t slash =
            scheme == std::string::npos ? std::string::npos
                                        : path_text.find('/', scheme + 3);
        if (slash == std::string::npos) {
            return std::nullopt;
        }
        path_text = path_text.substr(slash);
    }

    const std::size_t query = path_text.find_first_of("?#");
    if (query != std::string::npos) {
        path_text.erase(query);
    }

    std::error_code ec;
    const fs::path direct(path_text);
    if (fs::is_regular_file(direct, ec)) {
        const fs::path absolute = fs::absolute(direct, ec);
        if (!ec && isAllowedImagePath(absolute)) {
            return absolute;
        }
    }

    const std::string filename = direct.filename().string();
    if (filename.empty()) {
        return std::nullopt;
    }

    for (const auto& directory : allowedImageRoots()) {
        const fs::path candidate = directory / filename;
        if (fs::is_regular_file(candidate, ec)) {
            return fs::absolute(candidate);
        }
    }
    return std::nullopt;
}

class Renderer {
public:
    explicit Renderer(MarkdownPdfOptions options) : m_options(std::move(options)) {
        newPage();
    }

    void render(std::string_view markdown) {
        std::string normalized(markdown);
        std::string normal;
        normal.reserve(normalized.size());
        for (std::size_t i = 0; i < normalized.size(); ++i) {
            if (normalized[i] == '\r') {
                if (i + 1 < normalized.size() && normalized[i + 1] == '\n') {
                    continue;
                }
                normal.push_back('\n');
            } else {
                normal.push_back(normalized[i]);
            }
        }

        std::vector<std::string> lines;
        std::string current;
        for (const char c : normal) {
            if (c == '\n') {
                lines.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty() || lines.empty()) {
            lines.push_back(current);
        }

        std::vector<std::string> paragraph;
        auto flushParagraph = [&]() {
            if (paragraph.empty()) return;
            std::string joined;
            for (const auto& line : paragraph) {
                if (!joined.empty()) joined.push_back(' ');
                joined += line;
            }
            renderSpans(parseInline(joined, Face::Regular), 10.4, 0.0, kNavy);
            m_cursor += 2.0;
            paragraph.clear();
        };

        std::size_t i = 0;
        while (i < lines.size()) {
            const std::string line = lines[i];
            const std::string stripped = trim(line);
            if (stripped.empty()) {
                flushParagraph();
                m_cursor += 2.0;
                ++i;
                continue;
            }

            if (startsWith(stripped, "```") || startsWith(stripped, "~~~")) {
                flushParagraph();
                const std::string fence = stripped.substr(0, 3);
                std::vector<std::string> code;
                ++i;
                while (i < lines.size()) {
                    const std::string candidate = trim(lines[i]);
                    if (startsWith(candidate, fence)) {
                        ++i;
                        break;
                    }
                    code.push_back(lines[i]);
                    ++i;
                }
                renderCode(code);
                continue;
            }

            if (startsWith(stripped, "![")) {
                const std::size_t close = stripped.find(']');
                const std::size_t open = stripped.find('(', close == std::string::npos
                                                              ? 0
                                                              : close + 1);
                const std::size_t end = open == std::string::npos
                    ? std::string::npos
                    : stripped.rfind(')');
                if (close != std::string::npos && open != std::string::npos &&
                    end != std::string::npos && end > open) {
                    flushParagraph();
                    const std::string alt = stripped.substr(2, close - 2);
                    const std::string target =
                        trimmedTarget(stripped.substr(open + 1, end - open - 1));
                    renderImage(alt, target);
                    ++i;
                    continue;
                }
            }

            const int level = headingLevel(stripped);
            if (level > 0) {
                flushParagraph();
                const std::string title = trim(stripped.substr(
                    static_cast<std::size_t>(level)));
                renderHeading(level, title);
                ++i;
                continue;
            }

            if (stripped == "\\newpage" || stripped == "\\pagebreak" ||
                stripped == "<!-- pagebreak -->" || stripped == "<div style=\"page-break-after: always;\"></div>") {
                flushParagraph();
                newPage();
                ++i;
                continue;
            }

            if (isHorizontalRule(stripped)) {
                flushParagraph();
                renderHorizontalRule();
                ++i;
                continue;
            }

            if (stripped.front() == '>') {
                flushParagraph();
                std::vector<std::string> quote;
                while (i < lines.size()) {
                    const std::string candidate = trim(lines[i]);
                    if (candidate.empty() || candidate.front() != '>') break;
                    quote.push_back(trim(candidate.substr(1)));
                    ++i;
                }
                renderBlockquote(quote);
                continue;
            }

            if (isTableStart(lines, i)) {
                flushParagraph();
                std::vector<std::vector<std::string>> rows;
                rows.push_back(splitTableRow(lines[i]));
                i += 2;
                while (i < lines.size()) {
                    const std::string candidate = trim(lines[i]);
                    if (candidate.empty() || candidate.find('|') == std::string::npos) {
                        break;
                    }
                    rows.push_back(splitTableRow(candidate));
                    ++i;
                }
                renderTable(rows);
                continue;
            }

            std::size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') ++indent;
            const std::string list = trim(line);
            if (startsWith(list, "- ") || startsWith(list, "* ") ||
                startsWith(list, "+ ")) {
                flushParagraph();
                const std::size_t depth = indent / 2;
                renderListItem(std::string(1, list.front()),
                               trim(list.substr(2)), depth);
                ++i;
                continue;
            }
            const std::size_t dot = list.find(". ");
            if (dot != std::string::npos && dot > 0 && dot <= 4 &&
                std::all_of(list.begin(), list.begin() + static_cast<long>(dot),
                            [](char c) {
                                return std::isdigit(static_cast<unsigned char>(c)) != 0;
                            })) {
                flushParagraph();
                const std::size_t depth = indent / 2;
                renderListItem(list.substr(0, dot + 1),
                               trim(list.substr(dot + 2)), depth);
                ++i;
                continue;
            }

            paragraph.push_back(stripped);
            ++i;
        }
        flushParagraph();
    }

    std::vector<uint8_t> finish() {
        const std::size_t page_count = m_pages.size();
        for (std::size_t i = 0; i < page_count; ++i) {
            const std::string footer = fmt::format(
                "BT /F1 8 Tf 0.45 0.48 0.55 rg 1 0 0 1 {:.2f} {:.2f} Tm "
                "(BawalVision | Page {} of {}) Tj ET\n",
                kMarginLeft, 22.0, i + 1, page_count);
            m_pages[i].content += footer;
        }

        std::string out;
        out.reserve(64 * 1024);
        out += "%PDF-1.4\n";
        out += "%\xE2\xE3\xCF\xD3\n";

        std::vector<std::size_t> offsets;
        auto addObject = [&](const std::string& body) -> int {
            const int id = static_cast<int>(offsets.size()) + 1;
            offsets.push_back(out.size());
            out += fmt::format("{} 0 obj\n", id);
            out += body;
            out += "\nendobj\n";
            return id;
        };

        addObject("<< /Type /Catalog /Pages 2 0 R >>");

        std::string kids;
        for (std::size_t i = 0; i < page_count; ++i) {
            const int page_id = 7 + static_cast<int>(i * 2);
            kids += fmt::format("{} 0 R ", page_id);
        }
        addObject(fmt::format(
            "<< /Type /Pages /Count {} /Kids [{}] >>", page_count, kids));

        addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                  "/Encoding /WinAnsiEncoding >>");
        addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                  "/Encoding /WinAnsiEncoding >>");
        addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique "
                  "/Encoding /WinAnsiEncoding >>");
        addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Courier "
                  "/Encoding /WinAnsiEncoding >>");

        std::string xobjects;
        const int first_image_id = 7 + static_cast<int>(page_count * 2);
        for (std::size_t i = 0; i < m_images.size(); ++i) {
            xobjects += fmt::format("/Im{} {} 0 R ", i,
                                    first_image_id + static_cast<int>(i));
        }

        for (std::size_t i = 0; i < page_count; ++i) {
            const int page_id = 7 + static_cast<int>(i * 2);
            const int content_id = page_id + 1;
            addObject(fmt::format(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {:.2f} {:.2f}] "
                "/Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R "
                "/F4 6 0 R >> /XObject << {}>> >> /Contents {} 0 R >>",
                kPageWidth, kPageHeight, xobjects, content_id));
            addObject(fmt::format("<< /Length {} >>\nstream\n{}\nendstream",
                                  m_pages[i].content.size(),
                                  m_pages[i].content));
        }

        for (const auto& image : m_images) {
            const char* color_space = image.components == 1
                ? "/DeviceGray"
                : image.components == 4 ? "/DeviceCMYK" : "/DeviceRGB";
            std::string body = fmt::format(
                "<< /Type /XObject /Subtype /Image /Width {} /Height {} "
                "/ColorSpace {} /BitsPerComponent 8 /Filter /DCTDecode "
                "/Length {} >>\nstream\n",
                image.width, image.height, color_space, image.bytes.size());
            body.append(reinterpret_cast<const char*>(image.bytes.data()),
                        image.bytes.size());
            body += "\nendstream";
            addObject(body);
        }

        const std::string creation_date = pdfDateNow();
        const int info_id = addObject(fmt::format(
            "<< /Title ({}) /Author ({}) /Subject ({}) /Creator ({}) "
            "/Producer (BawalVision Markdown PDF Engine) /CreationDate ({}) >>",
            pdfEscape(m_options.title), pdfEscape(m_options.author),
            pdfEscape(m_options.subject), pdfEscape(m_options.creator),
            creation_date));

        const std::size_t xref_offset = out.size();
        out += fmt::format("xref\n0 {}\n", offsets.size() + 1);
        out += "0000000000 65535 f \n";
        for (const std::size_t offset : offsets) {
            out += fmt::format("{:010} 00000 n \n", offset);
        }
        out += fmt::format(
            "trailer\n<< /Size {} /Root 1 0 R /Info {} 0 R >>\n"
            "startxref\n{}\n%%EOF\n",
            offsets.size() + 1, info_id, xref_offset);

        return std::vector<uint8_t>(out.begin(), out.end());
    }

private:
    struct Page {
        std::string content;
    };

    void newPage() {
        m_pages.emplace_back();
        m_cursor = kMarginTop;
    }

    void ensureSpace(double height) {
        if (m_cursor + height > kPageHeight - kMarginBottom) {
            newPage();
        }
    }

    void drawText(double x, double baseline_top, double size, Face face,
                  const Rgb& color, std::string_view text) {
        if (text.empty()) return;
        m_pages.back().content += fmt::format(
            "BT /{} {:.2f} Tf {:.3f} {:.3f} {:.3f} rg "
            "1 0 0 1 {:.2f} {:.2f} Tm ({}) Tj ET\n",
            fontName(face), size, color.r, color.g, color.b, x,
            kPageHeight - baseline_top, pdfEscape(text));
    }

    void drawRect(double x, double top, double width, double height,
                  const Rgb& color) {
        m_pages.back().content += fmt::format(
            "q {:.3f} {:.3f} {:.3f} rg {:.2f} {:.2f} {:.2f} {:.2f} re f Q\n",
            color.r, color.g, color.b, x, kPageHeight - top - height, width,
            height);
    }

    void drawLine(double x1, double top1, double x2, double top2,
                  const Rgb& color, double line_width = 0.7) {
        m_pages.back().content += fmt::format(
            "q {:.3f} {:.3f} {:.3f} RG {:.2f} w {:.2f} {:.2f} m "
            "{:.2f} {:.2f} l S Q\n",
            color.r, color.g, color.b, line_width, x1, kPageHeight - top1, x2,
            kPageHeight - top2);
    }

    void renderSpans(const std::vector<Span>& spans, double size, double indent,
                     const Rgb& color) {
        const double left = kMarginLeft + indent;
        const double right = kPageWidth - kMarginRight;
        const double line_height = size * 1.47;
        const std::vector<Token> tokens = tokenize(spans);

        struct Placed {
            std::string text;
            Face face;
            double x;
        };
        std::vector<Placed> line;
        double x = left;
        bool pending_space = false;

        auto flushLine = [&]() {
            if (line.empty()) return;
            ensureSpace(line_height);
            const double baseline = m_cursor + size * 0.82;
            for (const auto& placed : line) {
                drawText(placed.x, baseline, size, placed.face, color, placed.text);
            }
            m_cursor += line_height;
            line.clear();
            x = left;
        };

        for (const auto& token : tokens) {
            if (token.space) {
                pending_space = !line.empty();
                continue;
            }
            const double word_width = measureText(token.text, token.face, size);
            std::string word_text = token.text;
            double placed_width = word_width;
            if (pending_space) {
                const double space_width = measureText(" ", token.face, size);
                if (!line.empty() && x + space_width + word_width > right) {
                    flushLine();
                } else if (!line.empty()) {
                    word_text.insert(word_text.begin(), ' ');
                    placed_width += space_width;
                }
                pending_space = false;
            }
            if (!line.empty() && x + placed_width > right) {
                flushLine();
                word_text = token.text;
                placed_width = word_width;
            }

            if (placed_width > right - left) {
                std::string chunk;
                for (const char ch : word_text) {
                    const std::string candidate = chunk + ch;
                    if (!chunk.empty() &&
                        measureText(candidate, token.face, size) > right - left) {
                        line.push_back({chunk, token.face, x});
                        x += measureText(chunk, token.face, size);
                        flushLine();
                        chunk.assign(1, ch);
                    } else {
                        chunk = candidate;
                    }
                }
                if (!chunk.empty()) {
                    const double chunk_width =
                        measureText(chunk, token.face, size);
                    if (!line.empty() && x + chunk_width > right) {
                        flushLine();
                    }
                    line.push_back({chunk, token.face, x});
                    x += chunk_width;
                }
            } else {
                line.push_back({word_text, token.face, x});
                x += placed_width;
            }
        }
        flushLine();
    }

    void renderHeading(int level, const std::string& text) {
        static constexpr std::array<double, 6> sizes{
            18.0, 15.0, 13.0, 11.5, 11.0, 10.5};
        const double size = sizes[static_cast<std::size_t>(std::clamp(level, 1, 6) - 1)];
        if (level == 1) {
            ensureSpace(38.0);
            drawRect(kMarginLeft, m_cursor, kContentWidth, 32.0, kNavy);
            const double text_width = measureText(text, Face::Bold, 13.0);
            const double x = kMarginLeft + std::max(12.0, (kContentWidth - text_width) / 2.0);
            drawText(x, m_cursor + 20.0, 13.0, Face::Bold, kWhite, text);
            m_cursor += 38.0;
            return;
        }
        m_cursor += 7.0;
        renderSpans(parseInline(text, Face::Bold), size, 0.0, kNavy);
        if (level == 2) {
            ensureSpace(8.0);
            drawLine(kMarginLeft, m_cursor + 2.0,
                     kPageWidth - kMarginRight, m_cursor + 2.0,
                     kBlue, 1.0);
        }
        m_cursor += 6.0;
    }

    void renderHorizontalRule() {
        ensureSpace(10.0);
        drawLine(kMarginLeft, m_cursor + 3.0, kPageWidth - kMarginRight,
                 m_cursor + 3.0, kLightBlue, 0.9);
        m_cursor += 11.0;
    }

    void renderListItem(const std::string& marker, const std::string& text,
                        std::size_t depth) {
        const double size = 10.4;
        const double indent = 15.0 + static_cast<double>(depth) * 14.0;
        ensureSpace(size * 1.5);
        const std::string bullet = marker == "-" || marker == "*" || marker == "+"
            ? std::string(1, static_cast<char>(0x95))
            : marker;
        drawText(kMarginLeft + indent - 12.0, m_cursor + size * 0.82, size,
                 Face::Bold, kBlue, bullet);
        renderSpans(parseInline(text, Face::Regular), size, indent, kNavy);
        m_cursor += 1.5;
    }

    void renderBlockquote(const std::vector<std::string>& lines) {
        m_cursor += 2.0;
        for (const auto& line : lines) {
            if (line.empty()) {
                m_cursor += 3.0;
                continue;
            }
            const int level = headingLevel(line);
            const double top = m_cursor;
            if (level > 0) {
                const double size = level == 1 ? 15.0 : 12.5;
                ensureSpace(size * 1.5);
                drawText(kMarginLeft + 17.0, m_cursor + size * 0.82, size,
                         Face::Bold, kNavy,
                         sanitizeUtf8(trim(line.substr(
                             static_cast<std::size_t>(level)))));
                m_cursor += size * 1.47;
            } else {
                renderSpans(parseInline(line, Face::Regular), 10.6, 15.0, kSlate);
            }
            drawLine(kMarginLeft + 4.0, top, kMarginLeft + 4.0, m_cursor - 2.0,
                     kBlue, 1.8);
        }
        m_cursor += 5.0;
    }

    void renderTable(const std::vector<std::vector<std::string>>& rows) {
        if (rows.empty()) return;
        constexpr double font_size = 8.2;
        constexpr double padding = 4.0;
        const double line_height = font_size * 1.34;
        std::size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;
        const double cell_width = kContentWidth / static_cast<double>(columns);

        struct PreparedCell {
            std::vector<std::string> lines;
        };
        struct PreparedRow {
            std::vector<PreparedCell> cells;
            double height{0.0};
        };

        std::vector<PreparedRow> prepared;
        prepared.reserve(rows.size());
        for (const auto& row : rows) {
            PreparedRow out;
            out.cells.resize(columns);
            std::size_t max_lines = 1;
            for (std::size_t c = 0; c < columns; ++c) {
                const std::string cell = c < row.size() ? row[c] : std::string{};
                out.cells[c].lines = wrapPlainText(
                    flattenSpans(parseInline(cell, Face::Regular)),
                    cell_width - padding * 2.0, Face::Regular, font_size);
                max_lines = std::max(max_lines, out.cells[c].lines.size());
            }
            out.height = static_cast<double>(max_lines) * line_height +
                         padding * 2.0;
            prepared.push_back(std::move(out));
        }

        auto drawRow = [&](const PreparedRow& row, bool header, bool alternate) {
            ensureSpace(row.height);
            const double top = m_cursor;
            if (header) {
                drawRect(kMarginLeft, top, kContentWidth, row.height, kHeaderFill);
            } else if (alternate) {
                drawRect(kMarginLeft, top, kContentWidth, row.height, kAltFill);
            }

            for (std::size_t c = 0; c < columns; ++c) {
                const double x = kMarginLeft + static_cast<double>(c) * cell_width;
                drawLine(x, top, x, top + row.height, kLightBlue, 0.5);
                const auto& cell = row.cells[c];
                double baseline = top + padding + font_size * 0.82;
                for (const auto& text_line : cell.lines) {
                    Rgb cell_color = header ? kNavy : kSlate;
                    Face cell_face = header ? Face::Bold : Face::Regular;
                    if (!header) {
                        if (text_line == "PASS" || text_line == "ACCEPTED") {
                            cell_color = kGreen;
                            cell_face = Face::Bold;
                        } else if (text_line == "REVIEW") {
                            cell_color = kAmber;
                            cell_face = Face::Bold;
                        } else if (text_line == "REJECT" || text_line == "FAIL" || text_line == "REJECTED") {
                            cell_color = kRed;
                            cell_face = Face::Bold;
                        }
                    }
                    drawText(x + padding, baseline, font_size,
                             cell_face, cell_color, text_line);
                    baseline += line_height;
                }
            }
            drawLine(kMarginLeft, top + row.height, kPageWidth - kMarginRight,
                     top + row.height, kLightBlue, 0.5);
            m_cursor = top + row.height;
        };

        for (std::size_t r = 0; r < prepared.size(); ++r) {
            if (m_cursor + prepared[r].height > kPageHeight - kMarginBottom) {
                newPage();
                if (r > 0) {
                    drawRow(prepared[0], true, false);
                }
            }
            drawRow(prepared[r], r == 0, r > 0 && (r % 2 == 0));
        }
        m_cursor += 7.0;
    }

    void renderCode(const std::vector<std::string>& lines) {
        constexpr double font_size = 8.4;
        constexpr double line_height = font_size * 1.42;
        ensureSpace(line_height + 8.0);
        for (const auto& line : lines) {
            const auto chunks = wrapMono(sanitizeUtf8(line),
                                         kContentWidth - 12.0, font_size);
            for (const auto& chunk : chunks) {
                ensureSpace(line_height + 2.0);
                drawRect(kMarginLeft, m_cursor, kContentWidth, line_height,
                         kCodeFill);
                drawText(kMarginLeft + 6.0, m_cursor + font_size * 0.82, font_size,
                         Face::Mono, kSlate, chunk);
                m_cursor += line_height;
            }
        }
        m_cursor += 6.0;
    }

    static std::string trimmedTarget(std::string_view target) {
        std::string value = trim(target);
        if (value.size() >= 2 && value.front() == '<' && value.back() == '>') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }

    void renderImage(const std::string& alt, const std::string& target) {
        std::vector<uint8_t> bytes;
        if (const std::optional<fs::path> resolved = resolveImageTarget(target);
            resolved.has_value()) {
            bytes = readBinaryFile(*resolved);
        } else {
            bytes = decodeDataUriImage(target);
        }
        if (!bytes.empty()) {
            const std::vector<int> size = jpegDimensions(bytes);
            if (size.size() == 3) {
                const double max_height = 290.0;
                const double scale = std::min(
                    {kContentWidth / static_cast<double>(size[0]),
                     max_height / static_cast<double>(size[1]), 1.0});
                const double draw_width = static_cast<double>(size[0]) * scale;
                const double draw_height = static_cast<double>(size[1]) * scale;
                ensureSpace(draw_height + 16.0);
                m_images.push_back(EmbeddedImage{
                    size[0], size[1], size[2], std::move(bytes)});
                const double x =
                    kMarginLeft + (kContentWidth - draw_width) / 2.0;
                const double top = m_cursor;
                drawRect(x - 1.0, top - 1.0, draw_width + 2.0,
                         draw_height + 2.0, kLightBlue);
                m_pages.back().content += fmt::format(
                    "q {:.2f} 0 0 {:.2f} {:.2f} {:.2f} cm /Im{} Do Q\n",
                    draw_width, draw_height, x,
                    kPageHeight - top - draw_height, m_images.size() - 1);
                m_cursor += draw_height + 12.0;
                return;
            }
        }
        renderSpans(parseInline(alt.empty() ? "[Image unavailable]"
                                            : "[Image: " + alt + "]",
                                Face::Italic),
                    9.5, 0.0, kMuted);
        m_cursor += 3.0;
    }

    static std::string pdfDateNow() {
        const std::time_t now = std::time(nullptr);
        std::tm tm_utc{};
#if defined(_WIN32)
        gmtime_s(&tm_utc, &now);
#else
        gmtime_r(&now, &tm_utc);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "D:%Y%m%d%H%M%SZ", &tm_utc);
        return buffer;
    }

    MarkdownPdfOptions m_options;
    std::vector<Page> m_pages;
    std::vector<EmbeddedImage> m_images;
    double m_cursor{kMarginTop};
};

std::mutex& fileMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::vector<uint8_t> MarkdownPdfGenerator::render(
    std::string_view markdown, const MarkdownPdfOptions& options) {
    Renderer renderer(options);
    renderer.render(markdown);
    return renderer.finish();
}

std::string MarkdownPdfGenerator::generatePdfFile(
    std::string_view markdown, const std::filesystem::path& output_path,
    const MarkdownPdfOptions& options) {
    if (output_path.empty()) return {};
    const std::vector<uint8_t> bytes = render(markdown, options);
    if (bytes.empty()) return {};

    std::error_code ec;
    if (output_path.has_parent_path()) {
        fs::create_directories(output_path.parent_path(), ec);
    }

    std::lock_guard<std::mutex> lock(fileMutex());
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return {};
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) return {};
    return output_path.string();
}

std::string MarkdownPdfGenerator::defaultOutputDir() {
    return "/tmp/bawal_reports";
}

} // namespace bawal::report
