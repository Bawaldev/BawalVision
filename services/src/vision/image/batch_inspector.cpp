#include "bawal/vision/image/batch_inspector.hpp"
#include "bawal/report/markdown_pdf.hpp"
#include "bawal/report/report_builder.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <fmt/core.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <omp.h>

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

/** `command -v <tool>` probe used to pick a TeX engine at runtime. */
bool commandAvailable(const char* tool) {
    return std::system(fmt::format("command -v {} >/dev/null 2>&1", tool).c_str()) == 0;
}

std::string replace_all(std::string str, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
    return str;
}

/** FNV-1a 64-bit over the batch payload: tamper-evident report fingerprint. */
class Fnv1a64 {
public:
    void update(std::string_view bytes) noexcept {
        for (const char c : bytes) {
            m_hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            m_hash *= kPrime;
        }
    }
    void updateU32(uint32_t value) noexcept {
        update(std::string_view(reinterpret_cast<const char*>(&value), sizeof(value)));
    }
    void updateF32(float value) noexcept {
        update(std::string_view(reinterpret_cast<const char*>(&value), sizeof(value)));
    }
    [[nodiscard]] std::string hex() const {
        return fmt::format("{:016x}", m_hash);
    }

private:
    static constexpr uint64_t kOffset = 1469598103934665603ULL;
    static constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t m_hash{kOffset};
};

/** UTC wall-clock string stamped into the report header and audit trail. */
std::string utcTimestampNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);
    return buffer;
}

/** Weighted 0..100 batch quality index; rejects carry a 25-point penalty. */
float qualityIndex(const bawal::vision::image::BatchQualityMetrics& q) noexcept {
    const float base = 100.0f * (0.45f * q.avg_similarity + 0.55f * q.avg_quality / 100.0f);
    const float reject_ratio = q.total_detections > 0
        ? static_cast<float>(q.reject_detections) / static_cast<float>(q.total_detections)
        : 0.0f;
    return std::clamp(base - 25.0f * reject_ratio, 0.0f, 100.0f);
}

const char* qualityIndexColor(float index) noexcept {
    if (index >= 85.0f) return "acceptedgreen";
    if (index >= 70.0f) return "reviewamber";
    return "rejectedred";
}

/** Renders a horizontal LaTeX bar scaled against the largest bucket count. */
std::string latexBar(uint64_t value, uint64_t max_value, const char* color,
                     double max_width_cm = 7.0) {
    const double ratio = max_value > 0
        ? static_cast<double>(value) / static_cast<double>(max_value)
        : 0.0;
    const double width = std::max(0.02, ratio * max_width_cm);
    return fmt::format("\\textcolor{{{}}}{{\\rule{{{:.3f}cm}}{{7pt}}}}", color, width);
}

} // namespace

namespace bawal::vision::image {

BatchReportSummary BatchInspector::processZipBuffer(
    const uint8_t* zip_data,
    size_t zip_size,
    bawal::vision::FoodClassifier& classifier,
    const fs::path& template_path,
    const GrafanaAnalytics* grafana)
{
    BatchReportSummary summary;
    if (!zip_data || zip_size == 0 || zip_size > kMaxZipBytes) {
        return summary;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    summary.batch_id = fmt::format("BATCH-{}", timestamp);
    summary.inspection_type = std::string(kBatchInspectionType);
    summary.generated_at = utcTimestampNow();
    summary.model_arch = std::string(classifier.getModelArchitecture());
    if (const char* omp_env = std::getenv("OMP_NUM_THREADS"); omp_env != nullptr && *omp_env != '\0') {
        const int requested_threads = std::atoi(omp_env);
        if (requested_threads > 0) {
            omp_set_num_threads(requested_threads);
        }
    }
    const int max_threads = omp_get_max_threads();
    summary.omp_threads = max_threads > 0 ? max_threads : 1;
#if defined(__ARM_NEON) || defined(__aarch64__)
    const char* simd_str = "ARM NEON";
#elif defined(__AVX2__)
    const char* simd_str = "AVX2";
#else
    const char* simd_str = "SIMD";
#endif
    summary.engine = fmt::format("OpenMP ({} threads) + {} vectorized YOLOv8s INT8",
                                 summary.omp_threads, simd_str);
    if (grafana != nullptr) {
        summary.grafana = *grafana;
    }

    const fs::path work_dir = fs::path("/tmp/bawal_batches") / summary.batch_id;
    const fs::path extract_dir = work_dir / "extracted";
    const fs::path annotated_dir = work_dir / "annotated";
    fs::create_directories(extract_dir);
    fs::create_directories(annotated_dir);

    const fs::path temp_zip = work_dir / "upload.zip";
    {
        std::ofstream out(temp_zip, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip_data), static_cast<std::streamsize>(zip_size));
    }

    std::string unzip_cmd = fmt::format("unzip -q -j \"{}\" -d \"{}\" 2>&1", 
                                        temp_zip.string(), extract_dir.string());
    int res = std::system(unzip_cmd.c_str());
    if (res != 0) {
        // Fallback 1: python3 zipfile module
        std::string py_cmd = fmt::format("python3 -m zipfile -e \"{}\" \"{}\" > /dev/null 2>&1",
                                         temp_zip.string(), extract_dir.string());
        res = std::system(py_cmd.c_str());
        if (res != 0) {
            // Fallback 2: tar -xf
            std::string tar_cmd = fmt::format("tar -xf \"{}\" -C \"{}\" > /dev/null 2>&1",
                                             temp_zip.string(), extract_dir.string());
            (void)std::system(tar_cmd.c_str());
        }
    }

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
            // Reset internal classifier tracker so independent frames do not influence each other
            classifier.resetTracker();

            std::vector<bawal::vision::DetectionResult> detections;
            (void)classifier.detect(loaded_frames[i], detections, 0.40f, 0.45f);
            item.detections = std::move(detections);
        }

        summary.items[i] = std::move(item);
    }

    // ------------------------------------------------------------------
    // Parallel annotation pass: overlay boxes, verdict badges and the
    // similarity/SIMD-quality readout for every tray.
    // ------------------------------------------------------------------
    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < summary.items.size(); ++i) {
        auto& item = summary.items[i];
        if (loaded_frames[i].empty()) continue;

        cv::Mat annotated = loaded_frames[i].clone();
        bool tray_rejected = false;

        for (auto& d : item.detections) {
            const cv::Rect rect = d.bbox.toCvRect();

            const QualityStatus status = classifyQuality(d);
            const float freshness = freshnessScore(d);

            cv::Scalar box_color(40, 200, 40);
            const char* status_badge = qualityStatusName(status);
            if (status == QualityStatus::Reject) {
                box_color = cv::Scalar(30, 30, 235);
            } else if (status == QualityStatus::Review) {
                box_color = cv::Scalar(0, 165, 255);
            }

            cv::rectangle(annotated, rect, box_color, 2);

            // Similarity (confidence) + SIMD food quality (freshness) badge.
            const std::string label = fmt::format("{} {:.0f}% [{} {:.0f}%]",
                                                  d.class_name, d.confidence * 100.0f,
                                                  status_badge, freshness);

            int baseline = 0;
            const cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                                       0.44, 1, &baseline);
            const int badge_y = std::max(rect.y - 4, text_size.height + 4);
            const cv::Point badge_tl(rect.x, badge_y - text_size.height - 4);
            const cv::Point badge_br(rect.x + text_size.width + 8, badge_y + 2);
            cv::rectangle(annotated, badge_tl, badge_br, box_color, cv::FILLED);
            cv::putText(annotated, label, cv::Point(rect.x + 4, badge_y - 2),
                        cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);

            if (status == QualityStatus::Reject) {
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

    // ------------------------------------------------------------------
    // SIMD quality analytics: one gather pass, then AVX2 vector reductions
    // per tray, per food class and for the whole batch.
    // ------------------------------------------------------------------
    std::unordered_map<std::string, ClassAccumulator> class_map;
    class_map.reserve(16);

    std::vector<float> batch_similarities;
    std::vector<float> batch_qualities;
    size_t approx_detections = 0;
    for (const auto& item : summary.items) approx_detections += item.detections.size();
    batch_similarities.reserve(approx_detections);
    batch_qualities.reserve(approx_detections);

    summary.quality_histogram.assign(10, 0);

    for (auto& item : summary.items) {
        std::vector<float> similarities;
        std::vector<float> qualities;
        std::vector<QualityStatus> statuses;
        similarities.reserve(item.detections.size());
        qualities.reserve(item.detections.size());
        statuses.reserve(item.detections.size());

        for (const auto& d : item.detections) {
            const QualityStatus status = classifyQuality(d);
            statuses.push_back(status);
            if (status == QualityStatus::Pass) item.pass_detections++;
            else if (status == QualityStatus::Review) item.review_detections++;
            else item.reject_detections++;

            similarities.push_back(std::clamp(d.confidence, 0.0f, 1.0f));
            qualities.push_back(freshnessScore(d));
        }

        if (!similarities.empty()) {
            const bawal::vision::QualityVectorStats stats =
                bawal::vision::computeQualityStatsSIMD(
                    similarities.data(), qualities.data(), similarities.size());
            item.avg_similarity = stats.mean_similarity;
            item.avg_quality = stats.mean_quality;
        }

        for (size_t k = 0; k < item.detections.size(); ++k) {
            batch_similarities.push_back(similarities[k]);
            batch_qualities.push_back(qualities[k]);

            const int bucket = std::min(9, static_cast<int>(qualities[k] / 10.0f));
            summary.quality_histogram[static_cast<size_t>(bucket)]++;

            ClassAccumulator& acc =
                class_map[std::string(item.detections[k].class_name)];
            acc.similarities.push_back(similarities[k]);
            acc.qualities.push_back(qualities[k]);
            switch (statuses[k]) {
                case QualityStatus::Pass: acc.pass++; break;
                case QualityStatus::Review: acc.review++; break;
                case QualityStatus::Reject: acc.reject++; break;
            }
        }
    }

    if (!batch_similarities.empty()) {
        const bawal::vision::QualityVectorStats stats =
            bawal::vision::computeQualityStatsSIMD(
                batch_similarities.data(), batch_qualities.data(),
                batch_similarities.size());
        summary.quality.total_detections = static_cast<uint32_t>(batch_similarities.size());
        summary.quality.avg_similarity = stats.mean_similarity;
        summary.quality.avg_quality = stats.mean_quality;
        summary.quality.min_quality = stats.min_quality;
        summary.quality.max_quality = stats.max_quality;
        summary.quality.stddev_quality = stats.std_quality;
    }
    for (const auto& item : summary.items) {
        summary.quality.pass_detections += item.pass_detections;
        summary.quality.review_detections += item.review_detections;
        summary.quality.reject_detections += item.reject_detections;
    }
    summary.quality.quality_index = qualityIndex(summary.quality);

    summary.class_breakdown.reserve(class_map.size());
    for (auto& [name, acc] : class_map) {
        BatchClassStats stats;
        assignFixedString(stats.class_name, name);
        stats.detections = static_cast<uint32_t>(acc.similarities.size());
        stats.pass_count = acc.pass;
        stats.review_count = acc.review;
        stats.reject_count = acc.reject;
        if (!acc.similarities.empty()) {
            const bawal::vision::QualityVectorStats vec =
                bawal::vision::computeQualityStatsSIMD(
                    acc.similarities.data(), acc.qualities.data(),
                    acc.similarities.size());
            stats.avg_similarity = vec.mean_similarity;
            stats.avg_quality = vec.mean_quality;
        }
        summary.class_breakdown.push_back(std::move(stats));
    }
    std::sort(summary.class_breakdown.begin(), summary.class_breakdown.end(),
              [](const BatchClassStats& a, const BatchClassStats& b) {
                  return a.detections > b.detections;
              });

    // Tamper-evident fingerprint over batch identity and every scored detection.
    Fnv1a64 checksum;
    checksum.update(summary.batch_id);
    checksum.update(summary.inspection_type);
    checksum.update(summary.generated_at);
    for (const auto& item : summary.items) {
        checksum.update(item.filename);
        checksum.updateU32(item.index);
        checksum.updateU32(item.accepted ? 1u : 0u);
        for (const auto& d : item.detections) {
            checksum.update(d.class_name);
            checksum.updateF32(d.confidence);
            checksum.updateF32(d.spoilage.discoloration_ratio);
        }
    }
    summary.report_checksum = checksum.hex();

    // ------------------------------------------------------------------
    // LaTeX master report assembly.
    // ------------------------------------------------------------------
    std::ifstream tmpl_file(template_path);
    std::string latex_body;
    if (tmpl_file.is_open()) {
        std::stringstream buf;
        buf << tmpl_file.rdbuf();
        latex_body = buf.str();
    }

    const size_t total_trays = summary.total_images;
    const double pass_rate = total_trays > 0
        ? static_cast<double>(summary.accepted_count) / static_cast<double>(total_trays) * 100.0
        : 0.0;

    std::string class_rows;
    for (const auto& cls : summary.class_breakdown) {
        const double class_pass_rate = cls.detections > 0
            ? static_cast<double>(cls.pass_count) / static_cast<double>(cls.detections) * 100.0
            : 0.0;
        class_rows += fmt::format(
            "{} & {} & {:.1f}\\% & {:.1f}\\% & {} / {} / {} & {:.1f}\\% \\\\\n",
            latexEscape(fixedStringView(cls.class_name)), cls.detections,
            cls.avg_similarity * 100.0f,
            cls.avg_quality, cls.pass_count, cls.review_count, cls.reject_count,
            class_pass_rate);
    }
    if (class_rows.empty()) {
        class_rows = "\\multicolumn{6}{c}{No food items detected in this batch} \\\\\n";
    }

    uint64_t histogram_max = 0;
    for (const uint32_t count : summary.quality_histogram) {
        histogram_max = std::max<uint64_t>(histogram_max, count);
    }
    std::string histogram_rows;
    for (int bucket = 9; bucket >= 0; --bucket) {
        const uint64_t count = summary.quality_histogram[static_cast<size_t>(bucket)];
        const char* color = bucket >= 8 ? "acceptedgreen"
                          : bucket >= 6 ? "barblue"
                          : bucket >= 4 ? "reviewamber"
                                        : "rejectedred";
        histogram_rows += fmt::format("{}--{} & {} & {} \\\\\n",
                                      bucket * 10, bucket * 10 + 10,
                                      latexBar(count, histogram_max, color), count);
    }

    std::string grafana_section;
    if (summary.grafana.available) {
        const GrafanaAnalytics& g = summary.grafana;
        grafana_section += fmt::format(
            "\\noindent\\textbf{{Fleet snapshot}} (datasource: {}; dashboard \\texttt{{bawalvision\\_analytics}})\\\\[2pt]\n"
            "\\begin{{tabular}}{{llll}}\n"
            "\\hline \\textbf{{Total inspections}} & {} & \\textbf{{Last 24h}} & {} \\\\\n"
            "\\textbf{{Tracked devices}} & {} & \\textbf{{Avg similarity}} & {:.1f}\\% \\\\\n"
            "\\textbf{{Fresh food rate}} & {:.1f}\\% & \\textbf{{Avg spoilage}} & {:.1f}\\% \\\\\n"
            "\\hline\n\\end{{tabular}}\n\n",
            latexEscape(kGrafanaDatasource), g.total_inspections, g.inspections_24h,
            g.tracked_devices, g.avg_similarity_pct, g.fresh_food_rate, g.avg_spoilage_pct);

        if (!g.class_stats.empty()) {
            grafana_section +=
                "\\vspace{0.2cm}\n\\noindent\\textbf{Per-class fleet quality}\\\\[2pt]\n"
                "\\begin{tabular}{lcccc}\n"
                "\\hline \\textbf{Class} & \\textbf{{Detections}} & \\textbf{{Avg similarity}} & \\textbf{{Avg spoilage}} & \\textbf{{Pass rate}} \\\\ \\hline\n";
            for (const auto& cls : g.class_stats) {
                grafana_section += fmt::format("{} & {} & {:.1f}\\% & {:.1f}\\% & {:.1f}\\% \\\\\n",
                                               latexEscape(fixedStringView(cls.class_name)),
                                               cls.detections, cls.avg_similarity_pct,
                                               cls.avg_spoilage_pct, cls.pass_rate);
            }
            grafana_section += "\\hline\n\\end{tabular}\n\n";
        }

        if (!g.daily.empty()) {
            uint64_t daily_max = 0;
            for (const auto& point : g.daily) daily_max = std::max(daily_max, point.inspections);
            grafana_section +=
                "\\vspace{0.2cm}\n\\noindent\\textbf{7-day inspection trend}\\\\[2pt]\n"
                "\\begin{tabular}{lcc}\n"
                "\\hline \\textbf{Day (UTC)} & \\textbf{Volume} & \\textbf{Pass rate} \\\\ \\hline\n";
            for (const auto& point : g.daily) {
                grafana_section += fmt::format(
                    "{} & {} \\hspace{{2pt}}{} & {:.1f}\\% \\\\\n",
                    latexEscape(fixedStringView(point.day)), point.inspections,
                    latexBar(point.inspections, daily_max, "barblue", 4.0), point.pass_rate);
            }
            grafana_section += "\\hline\n\\end{tabular}\n\n";
        }
        grafana_section += fmt::format(
            "\\vspace{{0.15cm}}\n\\noindent\\small This batch is persisted with pipeline "
            "\\texttt{{img-batch}} and device \\texttt{{BATCH\\_IMG\\_UPLOAD}}, so its trays "
            "feed the same Grafana panels as live camera inspections.\n");
    } else {
        grafana_section =
            "\\noindent\\textcolor{reviewamber}{\\textbf{PostgreSQL datasource unreachable "
            "at report time.}} The fleet analytics section is omitted; this batch PDF was "
            "still generated from the in-memory inspection results. The Grafana dashboard "
            "(\\texttt{bawalvision\\_analytics}) resumes automatically once the database is "
            "back online.\n";
    }

    std::string tray_sections;
    for (const auto& item : summary.items) {
        std::string verdict_str = item.accepted 
            ? "\\textcolor{acceptedgreen}{\\textbf{ACCEPTED}}" 
            : "\\textcolor{rejectedred}{\\textbf{REJECTED}}";

        std::string det_rows;
        int d_idx = 1;
        for (const auto& d : item.detections) {
            const QualityStatus status = classifyQuality(d);
            const float freshness_pct = freshnessScore(d);
            const char* status_color = status == QualityStatus::Pass ? "acceptedgreen"
                                     : status == QualityStatus::Review ? "reviewamber"
                                                                       : "rejectedred";

            det_rows += fmt::format("{} & {} & [{}, {}, {}, {}] & {:.1f}\\% & {:.1f}\\% & "
                                    "\\textcolor{{{}}}{{\\textbf{{{}}}}} \\\\\n",
                                    d_idx++, latexEscape(d.class_name),
                                    d.bbox.x, d.bbox.y, d.bbox.width, d.bbox.height,
                                    d.confidence * 100.0f, freshness_pct,
                                    status_color, qualityStatusName(status));
        }
        if (det_rows.empty()) {
            det_rows = "1 & None & [0, 0, 0, 0] & 0.0\\% & 0.0\\% & N/A \\\\\n";
        }

        const char* tray_avg_color = item.avg_quality >= 80.0f ? "acceptedgreen"
                                   : item.avg_quality >= 55.0f ? "reviewamber"
                                                               : "rejectedred";
        tray_sections += fmt::format(
            "\\newpage\n"
            "\\section*{{Tray \\#{}: {}}}\n"
            "\\noindent Status: {} \\quad Surface Temp: {:.1f}$^\\circ$C \\quad Safe Window: {:.1f}h\\\\\n"
            "\\noindent\\small Avg similarity: {:.1f}\\% \\quad "
            "\\textcolor{{{}}}{{Avg quality: {:.1f}\\%}} \\quad "
            "Detections: {} (P {} / R {} / X {})\\\\\n"
            "\\vspace{{0.2cm}}\n"
            "\\begin{{center}}\n"
            "  \\includegraphics[width=0.75\\linewidth,height=6.2cm,keepaspectratio]{{{}}}\n"
            "\\end{{center}}\n"
            "\\vspace{{0.2cm}}\n"
            "\\begin{{tabular}}{{clcccc}}\n"
            "\\hline \\textbf{{No}} & \\textbf{{Item}} & \\textbf{{Bounding Box}} & \\textbf{{Similarity}} & \\textbf{{SIMD Quality}} & \\textbf{{Status}} \\\\ \\hline\n"
            "{}"
            "\\hline\n"
            "\\end{{tabular}}\n"
            "\\vspace{{0.4cm}}\n",
            item.index, latexEscape(item.filename), verdict_str, item.surface_temp, item.t_safe_hours,
            item.avg_similarity * 100.0f, tray_avg_color, item.avg_quality,
            item.detections.size(), item.pass_detections, item.review_detections,
            item.reject_detections, item.annotated_image_path, det_rows
        );
    }

    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_time).count();
    summary.total_processing_time_ms = elapsed_ms;
    summary.throughput_images_per_sec = elapsed_ms > 0.0
        ? static_cast<double>(summary.total_images) / (elapsed_ms / 1000.0)
        : 0.0;

    latex_body = replace_all(latex_body, "#@BATCH_ID@", latexEscape(summary.batch_id));
    latex_body = replace_all(latex_body, "#@INSPECTION_TYPE@", latexEscape(summary.inspection_type));
    latex_body = replace_all(latex_body, "#@GENERATED_AT@", latexEscape(summary.generated_at));
    latex_body = replace_all(latex_body, "#@MODEL_ARCH@", latexEscape(summary.model_arch));
    latex_body = replace_all(latex_body, "#@ENGINE@", latexEscape(summary.engine));
    latex_body = replace_all(latex_body, "#@OMP_THREADS@", std::to_string(summary.omp_threads));
    latex_body = replace_all(latex_body, "#@PROCESSING_TIME@", fmt::format("{:.2f}", elapsed_ms));
    latex_body = replace_all(latex_body, "#@THROUGHPUT@",
                             fmt::format("{:.2f}", summary.throughput_images_per_sec));
    latex_body = replace_all(latex_body, "#@TOTAL_TRAYS@", std::to_string(summary.total_images));
    latex_body = replace_all(latex_body, "#@ACCEPTED_COUNT@", std::to_string(summary.accepted_count));
    latex_body = replace_all(latex_body, "#@REJECTED_COUNT@", std::to_string(summary.rejected_count));
    latex_body = replace_all(latex_body, "#@PASS_RATE@", fmt::format("{:.1f}", pass_rate));
    latex_body = replace_all(latex_body, "#@QUALITY_INDEX@",
                             fmt::format("{:.1f}", summary.quality.quality_index));
    latex_body = replace_all(latex_body, "#@QUALITY_INDEX_COLOR@",
                             qualityIndexColor(summary.quality.quality_index));
    latex_body = replace_all(latex_body, "#@AVG_SIMILARITY@",
                             fmt::format("{:.1f}", summary.quality.avg_similarity * 100.0f));
    latex_body = replace_all(latex_body, "#@AVG_QUALITY@",
                             fmt::format("{:.1f}", summary.quality.avg_quality));
    latex_body = replace_all(latex_body, "#@MIN_QUALITY@",
                             fmt::format("{:.1f}", summary.quality.min_quality));
    latex_body = replace_all(latex_body, "#@MAX_QUALITY@",
                             fmt::format("{:.1f}", summary.quality.max_quality));
    latex_body = replace_all(latex_body, "#@QUALITY_STDDEV@",
                             fmt::format("{:.2f}", summary.quality.stddev_quality));
    latex_body = replace_all(latex_body, "#@TOTAL_DETECTIONS@",
                             std::to_string(summary.quality.total_detections));
    latex_body = replace_all(latex_body, "#@PASS_DETECTIONS@",
                             std::to_string(summary.quality.pass_detections));
    latex_body = replace_all(latex_body, "#@REVIEW_DETECTIONS@",
                             std::to_string(summary.quality.review_detections));
    latex_body = replace_all(latex_body, "#@REJECT_DETECTIONS@",
                             std::to_string(summary.quality.reject_detections));
    latex_body = replace_all(latex_body, "#@CLASS_TABLE@", class_rows);
    latex_body = replace_all(latex_body, "#@QUALITY_HISTOGRAM@", histogram_rows);
    latex_body = replace_all(latex_body, "#@GRAFANA_SECTION@", grafana_section);
    latex_body = replace_all(latex_body, "#@CHECKSUM@", latexEscape(summary.report_checksum));
    latex_body = replace_all(latex_body, "#@TRAY_SECTIONS@", tray_sections);

    fs::path master_tex = work_dir / "master_report.tex";
    fs::path master_pdf = work_dir / "master_report.pdf";
    {
        std::ofstream out_tex(master_tex);
        out_tex << latex_body;
    }

    // Prefer pdflatex, then tectonic (single static binary). Without any TeX
    // engine the .tex source remains the deliverable and is still served.
    std::string compile_cmd;
    if (commandAvailable("pdflatex")) {
        compile_cmd = fmt::format(
            "pdflatex -interaction=nonstopmode -output-directory=\"{}\" \"{}\" > /dev/null 2>&1",
            work_dir.string(), master_tex.string());
    } else if (commandAvailable("tectonic")) {
        compile_cmd = fmt::format(
            "tectonic --outdir \"{}\" --chatter minimal \"{}\" > /dev/null 2>&1",
            work_dir.string(), master_tex.string());
    }
    if (!compile_cmd.empty()) {
        (void)std::system(compile_cmd.c_str());
    }

    summary.master_tex_path = master_tex.string();

    if (fs::is_regular_file(master_pdf)) {
        summary.master_pdf_path = master_pdf.string();
    } else {
        bawal::report::MarkdownPdfOptions options;
        options.title = fmt::format("BawalVision Batch Audit - {}", summary.batch_id);
        options.subject = "Batch food tray quality and freshness compliance";
        const std::string portable = bawal::report::MarkdownPdfGenerator::generatePdfFile(
            bawal::report::buildBatchMarkdown(summary), master_pdf, options);
        if (!portable.empty()) {
            summary.master_pdf_path = portable;
        }
    }

    return summary;
}

} // namespace bawal::vision::image
