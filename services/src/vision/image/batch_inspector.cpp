#include "bawal/vision/image/batch_inspector.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <fmt/core.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <omp.h>

namespace bawal::vision::image {

namespace fs = std::filesystem;

namespace {

/** Escape LaTeX special characters for pdflatex text-mode interpolation. */
std::string latexEscape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char c : input) {
        switch (c) {
            case '\\': out += "\\textbackslash{}"; break;
            case '{': out += "\\{"; break;
            case '}': out += "\\}"; break;
            case '_': out += "\\_"; break;
            case '%': out += "\\%"; break;
            case '&': out += "\\&"; break;
            case '#': out += "\\#"; break;
            case '$': out += "\\$"; break;
            case '^': out += "\\textasciicircum{}"; break;
            case '~': out += "\\textasciitilde{}"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // namespace

BatchReportSummary BatchInspector::processZipBuffer(
    const uint8_t* zip_data,
    size_t zip_size,
    bawal::vision::FoodClassifier& classifier,
    const fs::path& template_path)
{
    BatchReportSummary summary;
    if (!zip_data || zip_size == 0 || zip_size > kMaxZipBytes) {
        return summary;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    summary.batch_id = fmt::format("BATCH-{}", timestamp);

    const fs::path work_dir = fs::path("/tmp/bawal_batches") / summary.batch_id;
    const fs::path extract_dir = work_dir / "extracted";
    const fs::path annotated_dir = work_dir / "annotated";
    fs::create_directories(extract_dir);
    fs::create_directories(annotated_dir);

    const fs::path temp_zip = work_dir / "upload.zip";
    {
        std::ofstream out(temp_zip, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip_data), zip_size);
    }

    std::string unzip_cmd = fmt::format("unzip -q -j \"{}\" -d \"{}\" 2>&1", 
                                        temp_zip.string(), extract_dir.string());
    (void)std::system(unzip_cmd.c_str());

    std::vector<fs::path> image_files;
    size_t total_uncompressed_bytes = 0;

    for (const auto& entry : fs::directory_iterator(extract_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp") {
            total_uncompressed_bytes += entry.file_size();
            image_files.push_back(entry.path());
            if (image_files.size() >= kMaxImageFiles || total_uncompressed_bytes > kMaxExtractedBytes) {
                break;
            }
        }
    }

    if (image_files.empty()) {
        fs::remove_all(work_dir);
        return summary;
    }

    summary.total_images = image_files.size();
    summary.items.resize(image_files.size());

    std::vector<cv::Mat> loaded_frames(image_files.size());
    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < image_files.size(); ++i) {
        loaded_frames[i] = cv::imread(image_files[i].string(), cv::IMREAD_COLOR);
    }

    for (size_t i = 0; i < image_files.size(); ++i) {
        BatchItemResult item;
        item.index = static_cast<uint32_t>(i + 1);
        item.filename = image_files[i].filename().string();

        if (!loaded_frames[i].empty()) {
            // Reset tracker bawaan classifier agar frame independen tidak saling memengaruhi
            classifier.resetTracker();

            std::vector<bawal::vision::DetectionResult> detections;
            (void)classifier.detect(loaded_frames[i], detections, 0.40f, 0.45f);
            item.detections = std::move(detections);
        }

        summary.items[i] = std::move(item);
    }

    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < summary.items.size(); ++i) {
        auto& item = summary.items[i];
        if (loaded_frames[i].empty()) continue;

        cv::Mat annotated = loaded_frames[i].clone();
        bool tray_rejected = false;

        for (auto& d : item.detections) {
            // Gunakan CompactBBox toCvRect() dari classifier.hpp
            cv::Rect rect = d.bbox.toCvRect();
            cv::Scalar box_color = (d.spoilage.is_visually_spoiled)
                ? cv::Scalar(0, 0, 220)
                : cv::Scalar(0, 200, 0);

            cv::rectangle(annotated, rect, box_color, 2);

            std::string label = fmt::format("{} {:.0f}%", d.class_name, d.confidence * 100.0f);
            cv::putText(annotated, label, cv::Point(rect.x, std::max(20, rect.y - 5)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, box_color, 2);

            if (d.spoilage.is_visually_spoiled) {
                tray_rejected = true;
            }
        }

        item.accepted = !tray_rejected && !item.detections.empty();

        fs::path out_annotated = annotated_dir / fmt::format("annotated-{}.jpg", item.index);
        cv::imwrite(out_annotated.string(), annotated, {cv::IMWRITE_JPEG_QUALITY, 85});
        item.annotated_image_path = out_annotated.string();
    }

    for (const auto& it : summary.items) {
        if (it.accepted) summary.accepted_count++;
        else summary.rejected_count++;
    }

    std::ifstream tmpl_file(template_path);
    std::string latex_body;
    if (tmpl_file.is_open()) {
        std::stringstream buf;
        buf << tmpl_file.rdbuf();
        latex_body = buf.str();
    }

    std::string tray_sections;
    for (const auto& item : summary.items) {
        std::string verdict_str = item.accepted 
            ? "\\textcolor{acceptedgreen}{\\textbf{ACCEPTED}}" 
            : "\\textcolor{rejectedred}{\\textbf{REJECTED}}";

        std::string det_rows;
        int d_idx = 1;
        for (const auto& d : item.detections) {
            float freshness = 1.0f - d.spoilage.discoloration_ratio;
            det_rows += fmt::format("{} & {} & [{}, {}, {}, {}] & {:.1f}\\% & {:.2f} \\\\\n",
                                    d_idx++, latexEscape(d.class_name),
                                    d.bbox.x, d.bbox.y, d.bbox.width, d.bbox.height,
                                    d.confidence * 100.0f, freshness);
        }
        if (det_rows.empty()) det_rows = "1 & None & [0, 0, 0, 0] & 0.0\\% & 0.00 \\\\\n";

        tray_sections += fmt::format(
            "\\newpage\n"
            "\\section*{{Tray \\#{}: {}}}\n"
            "\\noindent Status: {} \\quad Surface Temp: {:.1f}$^\\circ$C \\quad Safe Window: {:.1f}h\\\\\n"
            "\\vspace{{0.2cm}}\n"
            "\\begin{{center}}\n"
            "  \\includegraphics[width=0.75\\linewidth,height=6.5cm,keepaspectratio]{{{}}}\n"
            "\\end{{center}}\n"
            "\\vspace{{0.2cm}}\n"
            "\\begin{{tabular}}{{clccc}}\n"
            "\\hline \\textbf{{No}} & \\textbf{{Item}} & \\textbf{{Bounding Box}} & \\textbf{{Conf}} & \\textbf{{Freshness}} \\\\ \\hline\n"
            "{}"
            "\\hline\n"
            "\\end{{tabular}}\n"
            "\\vspace{{0.4cm}}\n",
            item.index, latexEscape(item.filename), verdict_str, item.surface_temp, item.t_safe_hours,
            item.annotated_image_path, det_rows
        );
    }

    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    replace_all(latex_body, "#@BATCH_ID@", latexEscape(summary.batch_id));
    replace_all(latex_body, "#@TOTAL_TRAYS@", std::to_string(summary.total_images));
    replace_all(latex_body, "#@ACCEPTED_COUNT@", std::to_string(summary.accepted_count));
    replace_all(latex_body, "#@REJECTED_COUNT@", std::to_string(summary.rejected_count));
    replace_all(latex_body, "#@TRAY_SECTIONS@", tray_sections);

    fs::path master_tex = work_dir / "master_report.tex";
    fs::path master_pdf = work_dir / "master_report.pdf";
    {
        std::ofstream out_tex(master_tex);
        out_tex << latex_body;
    }

    std::string pdflatex_cmd = fmt::format(
        "pdflatex -interaction=nonstopmode -output-directory=\"{}\" \"{}\" > /dev/null 2>&1",
        work_dir.string(), master_tex.string());
    (void)std::system(pdflatex_cmd.c_str());

    const auto end_time = std::chrono::steady_clock::now();
    summary.total_processing_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    if (fs::is_regular_file(master_pdf)) {
        summary.master_pdf_path = master_pdf.string();
    }

    return summary;
}

} // namespace bawal::vision::image