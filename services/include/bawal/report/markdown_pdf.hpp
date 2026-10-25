#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::report {

struct MarkdownPdfOptions {
    std::string title{"BawalVision Report"};
    std::string author{"BawalVision"};
    std::string subject{"Inspection Audit"};
    std::string creator{"BawalVision C++ report engine"};
};

class MarkdownPdfGenerator {
public:
    [[nodiscard]] static std::vector<uint8_t> render(
        std::string_view markdown,
        const MarkdownPdfOptions& options = {});

    [[nodiscard]] static std::string generatePdfFile(
        std::string_view markdown,
        const std::filesystem::path& output_path,
        const MarkdownPdfOptions& options = {});

    [[nodiscard]] static std::string defaultOutputDir();
};

} // namespace bawal::report
