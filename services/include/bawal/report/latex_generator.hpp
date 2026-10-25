#pragma once

#include "bawal/backend/state.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace bawal::report {

/**
 * @brief Autonomous LaTeX-to-PDF compilation engine for food inspection audits.
 *
 * Populates formal LaTeX templates with multimodal edge telemetry, YOLO bounding box
 * coordinates, freshness scores, and cryptographic checksums, then invokes `pdflatex`
 * in batch mode to generate competition-grade compliance documents.
 */
class LatexPdfGenerator {
public:
    /**
     * @brief Synthesizes a populated LaTeX (.tex) source file from inspection telemetry.
     *
     * Injects frame metadata, detection bounding boxes, surface temperatures, and
     * late-fusion shelf-life estimates into the target template tokens.
     *
     * @param entry Inspection record containing bounding boxes, labels, and sensor telemetry.
     * @param template_path Filesystem path to the input LaTeX template (.tex.in).
     * @return std::string Path to the generated .tex file in the temporary directory,
     *                     or empty string on I/O failure.
     */
    [[nodiscard]] static std::string generateReportTex(
        const backend::InspectionHistoryEntry& entry,
        const std::filesystem::path& template_path = {});

    /**
     * @brief Renders the inspection record as a portable Markdown audit document.
     *
     * The Markdown artifact is used by the built-in PDF engine and is also the
     * canonical representation consumed by the web report exporter.
     */
    [[nodiscard]] static std::string generateReportMarkdown(
        const backend::InspectionHistoryEntry& entry);

    /**
     * @brief Compiles inspection records into an official PDF report using pdflatex.
     *
     * Invokes `pdflatex` via a non-blocking subprocess with the generated .tex file.
     * Falls back to returning the raw .tex path if `pdflatex` is missing on the host.
     *
     * @param entry Inspection record containing bounding boxes, labels, and sensor telemetry.
     * @param template_path Filesystem path to the input LaTeX template (.tex.in).
     * @return std::string Path to the compiled .pdf artifact ready for download,
     *                     or the fallback .tex path if compilation fails.
     */
    [[nodiscard]] static std::string generateReportPdf(
        const backend::InspectionHistoryEntry& entry,
        const std::filesystem::path& template_path = {});
};

} // namespace bawal::report