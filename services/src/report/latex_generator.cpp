#include "bawal/report/latex_generator.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#include <fmt/core.h>

namespace bawal::report {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kTemplateName = "inspection_report.tex.in";
constexpr std::string_view kReportDir = "/tmp/bawal_reports";

std::mutex g_generate_mutex;

void replaceAll(std::string& str, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

/** Escape LaTeX special characters in runtime text (labels, paths, telemetry). */
std::string escapeLatex(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '\\': escaped += "\\textbackslash{}"; break;
            case '&': escaped += "\\&"; break;
            case '%': escaped += "\\%"; break;
            case '$': escaped += "\\$"; break;
            case '#': escaped += "\\#"; break;
            case '_': escaped += "\\_"; break;
            case '{': escaped += "\\{"; break;
            case '}': escaped += "\\}"; break;
            case '~': escaped += "\\textasciitilde{}"; break;
            case '^': escaped += "\\textasciicircum{}"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

fs::path envPath(const char* key) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') return {};
    return fs::path(value);
}

/** Find the report template relative to the backend working directory or binary. */
fs::path resolveTemplatePath(const fs::path& provided) {
    if (!provided.empty() && fs::is_regular_file(provided)) {
        return provided;
    }
    if (const fs::path from_env = envPath("BAWAL_REPORT_TEMPLATE");
        !from_env.empty() && fs::is_regular_file(from_env)) {
        return from_env;
    }

    std::error_code ec;
    const fs::path exe_dir = fs::canonical("/proc/self/exe", ec).parent_path();

    const fs::path candidates[] = {
        fs::path("./services/templates") / kTemplateName,
        fs::path("../services/templates") / kTemplateName,
        fs::path("../../services/templates") / kTemplateName,
        fs::path("./templates") / kTemplateName,
        fs::path("../templates") / kTemplateName,
        fs::path("/app/services/templates") / kTemplateName,
        exe_dir / "services/templates" / kTemplateName,
        exe_dir / "templates" / kTemplateName,
        exe_dir / "../templates" / kTemplateName,
        exe_dir / "../../templates" / kTemplateName,
        exe_dir / "../../../services/templates" / kTemplateName,
        provided,
    };
    for (const fs::path& candidate : candidates) {
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

/**
 * Resolve the `/inspections/<file>` URL stored in the audit entry to the real
 * JPEG on disk (the backend serves it from ./data/inspections).
 */
fs::path resolveImagePath(std::string_view url_path) {
    if (url_path.empty()) return {};

    std::error_code ec;
    const fs::path direct(url_path);
    if (direct.is_absolute() && fs::is_regular_file(direct, ec)) {
        return direct;
    }

    const std::string filename = direct.filename().string();
    if (filename.empty()) return {};

    const fs::path directories[] = {
        envPath("BAWAL_IMAGE_DIR"),
        fs::path("./data/inspections"),
        fs::path("/app/data/inspections"),
    };
    for (const fs::path& directory : directories) {
        if (directory.empty()) continue;
        const fs::path candidate = directory / filename;
        if (fs::is_regular_file(candidate, ec)) {
            return fs::absolute(candidate);
        }
    }
    return {};
}

std::string buildDetectionRows(const backend::InspectionHistoryEntry& entry) {
    if (entry.detections.empty()) {
        return "1 & \\textit{No Food Detected} & [0, 0, 0, 0] & 0.0\\% & 0.00 & "
               "\\textcolor{rejectedred}{\\textbf{FAIL}} \\\\\n";
    }

    std::string rows;
    int index = 1;
    for (const auto& detection : entry.detections) {
        const std::string status_cell = (detection.freshness_score >= 0.70f)
            ? "\\textcolor{acceptedgreen}{\\textbf{PASS}}"
            : "\\textcolor{rejectedred}{\\textbf{FAIL}}";

        rows += fmt::format(
            "{} & \\textbf{{{}}} & [{}, {}, {}, {}] & {:.1f}\\% & {:.2f} & {} \\\\\n",
            index++,
            escapeLatex(detection.class_name.empty() ? "unknown" : detection.class_name),
            detection.bbox[0], detection.bbox[1], detection.bbox[2], detection.bbox[3],
            detection.confidence * 100.0f,
            detection.freshness_score,
            status_cell);
    }
    return rows;
}

} // namespace

std::string LatexPdfGenerator::generateReportTex(
    const backend::InspectionHistoryEntry& entry,
    const fs::path& template_path) {
    const fs::path resolved_template = resolveTemplatePath(template_path);
    if (resolved_template.empty()) {
        std::cerr << "[REPORT] LaTeX template not found (set BAWAL_REPORT_TEMPLATE)\n";
        return {};
    }

    std::ifstream template_file(resolved_template);
    if (!template_file.is_open()) {
        std::cerr << "[REPORT] Unable to open template " << resolved_template << '\n';
        return {};
    }

    std::stringstream buffer;
    buffer << template_file.rdbuf();
    std::string content = buffer.str();

    const fs::path image = resolveImagePath(entry.image_path);
    const std::string verdict_color =
        (entry.verdict == "ACCEPTED") ? "acceptedgreen" : "rejectedred";
    const std::string short_hash = entry.checksum_sha256.size() >= 16
        ? entry.checksum_sha256.substr(0, 16)
        : "N/A-STATION-DEV";

    replaceAll(content, "#@FRAME_ID@", std::to_string(entry.frame_id));
    replaceAll(content, "#@DEVICE_ID@", escapeLatex(
        entry.device_id.empty() ? "ESP32S3_UNKNOWN" : entry.device_id));
    replaceAll(content, "#@TIMESTAMP@", escapeLatex(
        entry.timestamp.empty() ? "N/A" : entry.timestamp));
    replaceAll(content, "#@ENGINE_PIPELINE@", escapeLatex(
        entry.engine_pipeline.empty() ? "YOLOv8-INT8 + OpenCV SIMD" : entry.engine_pipeline));
    replaceAll(content, "#@T_SAFE_HOURS@", fmt::format("{:.1f}", entry.t_safe_hours));
    replaceAll(content, "#@VERDICT@", escapeLatex(
        entry.verdict.empty() ? "UNKNOWN" : entry.verdict));
    replaceAll(content, "#@VERDICT_BG@", verdict_color);
    replaceAll(content, "#@IMAGE_PATH@",
               image.empty() ? "_missing_image_" : image.string());
    replaceAll(content, "#@SURFACE_TEMP@", fmt::format("{:.1f}", entry.surface_temp));
    replaceAll(content, "#@LUX_LUMINANCE@", fmt::format("{:.1f}", entry.lux_luminance));
    replaceAll(content, "#@COLOR_TEMP@", std::to_string(entry.color_temperature));
    replaceAll(content, "#@WHITE_BALANCE@", escapeLatex(entry.white_balance_status));
    replaceAll(content, "#@SPOILAGE_RISK@", fmt::format("{:.1f}", entry.spoilage_risk));
    replaceAll(content, "#@TRAY_COVERAGE@", fmt::format("{:.1f}", entry.tray_coverage));
    replaceAll(content, "#@DETECTIONS_ROWS@", buildDetectionRows(entry));
    replaceAll(content, "#@CHECKSUM@", escapeLatex(short_hash));

    std::lock_guard<std::mutex> lock(g_generate_mutex);
    std::error_code ec;
    fs::create_directories(kReportDir, ec);

    const fs::path out_tex = fs::path(kReportDir) / fmt::format("report_{}.tex", entry.frame_id);
    std::ofstream out_file(out_tex);
    if (!out_file.is_open()) {
        std::cerr << "[REPORT] Unable to write " << out_tex << '\n';
        return {};
    }
    out_file << content;
    return out_tex.string();
}

std::string LatexPdfGenerator::generateReportPdf(
    const backend::InspectionHistoryEntry& entry,
    const fs::path& template_path) {
    const std::string tex_path = generateReportTex(entry, template_path);
    if (tex_path.empty()) {
        return {};
    }

    // Prefer pdflatex, then tectonic (single binary, XeTeX based). Without a
    // TeX engine the generated .tex source is the deliverable.
    std::string command;
    if (std::system("command -v pdflatex >/dev/null 2>&1") == 0) {
        command = fmt::format(
            "pdflatex -interaction=nonstopmode -halt-on-error -output-directory={} \"{}\" "
            ">/dev/null 2>&1",
            std::string(kReportDir), tex_path);
    } else if (std::system("command -v tectonic >/dev/null 2>&1") == 0) {
        command = fmt::format(
            "tectonic --outdir {} --chatter minimal \"{}\" >/dev/null 2>&1",
            std::string(kReportDir), tex_path);
    } else {
        return tex_path;
    }

    const fs::path pdf_path = fs::path(kReportDir) / fmt::format("report_{}.pdf", entry.frame_id);
    std::lock_guard<std::mutex> lock(g_generate_mutex);
    const int status = std::system(command.c_str());
    std::error_code ec;
    if (status == 0 && fs::is_regular_file(pdf_path, ec)) {
        return pdf_path.string();
    }
    return tex_path;
}

} // namespace bawal::report
