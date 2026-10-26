#include "bawal/report/report_builder.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "bawal/server/inspection_dto.hpp"
#include "bawal/vision/image/batch_inspector.hpp"

namespace {

std::string markdownEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

namespace bawal::report {

std::string buildBatchMarkdown(
    const vision::image::BatchReportSummary& summary) {
    using vision::image::freshnessScore;
    using vision::image::QualityStatus;
    using vision::image::qualityStatusName;
    using vision::image::fixedStringView;

    const double pass_rate = summary.total_images > 0
        ? static_cast<double>(summary.accepted_count) /
              static_cast<double>(summary.total_images) * 100.0
        : 0.0;

    std::ostringstream md;
    md << "# BawalVision Batch Food Tray Audit\n\n";
    md << "**High-Throughput Parallel Multimodal Inspection Engine**\n\n";
    md << "| Field | Value |\n";
    md << "| --- | --- |\n";
    md << "| Batch ID | " << markdownEscape(summary.batch_id) << " |\n";
    md << "| Inspection Type | " << markdownEscape(summary.inspection_type)
       << " |\n";
    md << "| Model Architecture | " << markdownEscape(summary.model_arch)
       << " |\n";
    md << "| Parallel Engine | " << markdownEscape(summary.engine) << " |\n";
    md << "| OpenMP Workers | " << summary.omp_threads << " threads |\n";
    md << "| Total Trays | " << summary.total_images << " |\n";
    md << "| Accepted | " << summary.accepted_count << " |\n";
    md << "| Rejected | " << summary.rejected_count << " |\n";
    md << "| Pass Rate | " << fmt::format("{:.1f}%", pass_rate) << " |\n";
    md << "| Processing Time | "
       << fmt::format("{:.2f} ms", summary.total_processing_time_ms) << " |\n";
    md << "| Throughput | "
       << fmt::format("{:.2f} trays/s", summary.throughput_images_per_sec)
       << " |\n";
    md << "| Report Checksum | `" << markdownEscape(summary.report_checksum)
       << "` |\n\n";

    md << "## Executive Summary\n\n";
    md << "| Metric | Value |\n";
    md << "| --- | --- |\n";
    md << "| Batch Quality Index | "
       << fmt::format("{:.1f} / 100", summary.quality.quality_index) << " |\n";
    md << "| Avg Similarity | "
       << fmt::format("{:.1f}%", summary.quality.avg_similarity * 100.0f)
       << " |\n";
    md << "| Avg Food Quality | "
       << fmt::format("{:.1f}%", summary.quality.avg_quality) << " |\n";
    md << "| Quality Range | "
       << fmt::format("{:.1f}% - {:.1f}%", summary.quality.min_quality,
                      summary.quality.max_quality)
       << " |\n";
    md << "| Quality Std Dev | "
       << fmt::format("{:.2f}", summary.quality.stddev_quality) << " |\n";
    md << "| Total Detections | " << summary.quality.total_detections
       << " |\n";
    md << "| PASS / REVIEW / REJECT | " << summary.quality.pass_detections
       << " / " << summary.quality.review_detections << " / "
       << summary.quality.reject_detections << " |\n\n";

    md << "## Detection Quality Analytics\n\n";
    md << "| Food Class | Detections | Avg Similarity | Avg Quality | P / R / X | Pass Rate |\n";
    md << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
    if (summary.class_breakdown.empty()) {
        md << "| _No food items detected_ | 0 | 0.0% | 0.0% | 0 / 0 / 0 | 0.0% |\n";
    } else {
        for (const auto& cls : summary.class_breakdown) {
            const double class_pass_rate = cls.detections > 0
                ? static_cast<double>(cls.pass_count) /
                      static_cast<double>(cls.detections) * 100.0
                : 0.0;
            md << "| " << markdownEscape(fixedStringView(cls.class_name))
               << " | " << cls.detections << " | "
               << fmt::format("{:.1f}%", cls.avg_similarity * 100.0f) << " | "
               << fmt::format("{:.1f}%", cls.avg_quality) << " | "
               << cls.pass_count << " / " << cls.review_count << " / "
               << cls.reject_count << " | "
               << fmt::format("{:.1f}%", class_pass_rate) << " |\n";
        }
    }
    md << "\n";

    md << "## SIMD Freshness Distribution\n\n";
    md << "| Quality Bucket | Count |\n";
    md << "| --- | ---: |\n";
    for (int bucket = 9; bucket >= 0; --bucket) {
        const std::size_t index = static_cast<std::size_t>(bucket);
        const uint32_t count = index < summary.quality_histogram.size()
            ? summary.quality_histogram[index]
            : 0;
        md << "| " << bucket * 10 << "-" << bucket * 10 + 10 << "% | " << count
           << " |\n";
    }
    md << "\n";

    if (summary.grafana.available) {
        const vision::image::GrafanaAnalytics& grafana = summary.grafana;
        md << "## Grafana Fleet Analytics\n\n";
        md << "| Metric | Value |\n";
        md << "| --- | --- |\n";
        md << "| Total Inspections | " << grafana.total_inspections << " |\n";
        md << "| Last 24h | " << grafana.inspections_24h << " |\n";
        md << "| Tracked Devices | " << grafana.tracked_devices << " |\n";
        md << "| Avg Similarity | "
           << fmt::format("{:.1f}%", grafana.avg_similarity_pct) << " |\n";
        md << "| Fresh Food Rate | "
           << fmt::format("{:.1f}%", grafana.fresh_food_rate) << " |\n";
        md << "| Avg Spoilage | "
           << fmt::format("{:.1f}%", grafana.avg_spoilage_pct) << " |\n\n";

        if (!grafana.class_stats.empty()) {
            md << "| Fleet Class | Detections | Avg Similarity | Avg Spoilage | Pass Rate |\n";
            md << "| --- | ---: | ---: | ---: | ---: |\n";
            for (const auto& cls : grafana.class_stats) {
                md << "| " << markdownEscape(fixedStringView(cls.class_name))
                   << " | " << cls.detections << " | "
                   << fmt::format("{:.1f}%", cls.avg_similarity_pct) << " | "
                   << fmt::format("{:.1f}%", cls.avg_spoilage_pct) << " | "
                   << fmt::format("{:.1f}%", cls.pass_rate) << " |\n";
            }
            md << "\n";
        }
    } else {
        md << "## Grafana Fleet Analytics\n\n";
        md << "> PostgreSQL datasource unreachable at report time. The fleet "
              "analytics section is omitted; this batch PDF was still generated "
              "from the in-memory inspection results.\n\n";
    }

    for (const auto& item : summary.items) {
        md << "## Tray #" << item.index << ": "
           << markdownEscape(item.filename) << "\n\n";
        md << "| Field | Value |\n";
        md << "| --- | --- |\n";
        md << "| Status | " << (item.accepted ? "ACCEPTED" : "REJECTED")
           << " |\n";
        md << "| Surface Temperature | "
           << fmt::format("{:.1f} C", item.surface_temp) << " |\n";
        md << "| Safe Window | "
           << fmt::format("{:.1f} hours", item.t_safe_hours) << " |\n";
        md << "| Avg Similarity | "
           << fmt::format("{:.1f}%", item.avg_similarity * 100.0f) << " |\n";
        md << "| Avg Quality | " << fmt::format("{:.1f}%", item.avg_quality)
           << " |\n";
        md << "| Detections (P/R/X) | " << item.detections.size() << " ("
           << item.pass_detections << "/" << item.review_detections << "/"
           << item.reject_detections << ") |\n\n";

        if (!item.annotated_image_path.empty()) {
            md << "![Tray " << item.index << " annotated evidence]("
               << item.annotated_image_path << ")\n\n";
        }

        md << "| No | Item | Bounding Box | Similarity | SIMD Quality | Status |\n";
        md << "| ---: | --- | --- | ---: | ---: | --- |\n";
        if (item.detections.empty()) {
            md << "| 1 | _No food detected_ | [0, 0, 0, 0] | 0.0% | 0.0% | N/A |\n";
        } else {
            int index = 1;
            for (const auto& detection : item.detections) {
                const QualityStatus status =
                    vision::image::classifyQuality(detection);
                md << "| " << index++ << " | "
                   << markdownEscape(detection.class_name) << " | ["
                   << detection.bbox.x << ", " << detection.bbox.y << ", "
                   << detection.bbox.width << ", " << detection.bbox.height
                   << "] | "
                   << fmt::format("{:.1f}%", detection.confidence * 100.0f)
                   << " | "
                   << fmt::format("{:.1f}%", freshnessScore(detection)) << " | "
                   << qualityStatusName(status) << " |\n";
            }
        }
        md << "\n";
    }

    md << "---\n\n";
    md << "_Generated automatically by the BawalVision C++ batch audit engine._\n";
    return md.str();
}

std::string buildLambdaInspectionMarkdown(
    const server::InspectionResponse& inspection) {
    std::ostringstream md;
    md << "# BawalVision Lambda Inspection Audit\n\n";
    md << "**Serverless MBG Food Quality & Freshness Report**\n\n";
    md << "| Field | Value |\n";
    md << "| --- | --- |\n";
    md << "| Frame ID | " << inspection.frame_id << " |\n";
    md << "| Device | " << (inspection.device_id.empty()
                               ? "LAMBDA_SERVERLESS"
                               : inspection.device_id)
       << " |\n";
    md << "| Engine | " << (inspection.engine_pipeline.empty()
                               ? "ONNX_INT8_LAMBDA"
                               : inspection.engine_pipeline)
       << " |\n";
    md << "| Status | " << (inspection.status.empty() ? "PROCESSED"
                                                      : inspection.status)
       << " |\n";
    md << "| Processing Time | "
       << fmt::format("{:.2f}", inspection.processing_time_ms) << " ms |\n\n";

    if (!inspection.image_path.empty()) {
        md << "## Visual Evidence\n\n";
        md << "![Annotated inspection evidence](" << inspection.image_path
           << ")\n\n";
    }

    md << "## Detection Log\n\n";
    md << "| No | Item | Bounding Box | Confidence |\n";
    md << "| ---: | --- | --- | ---: |\n";
    if (inspection.detections.empty()) {
        md << "| 1 | _No Food Detected_ | [0, 0, 0, 0] | 0.0% |\n";
    } else {
        int index = 1;
        for (const auto& detection : inspection.detections) {
            md << "| " << index++ << " | "
               << (detection.class_name.empty() ? "unknown"
                                                : detection.class_name)
               << " | [" << detection.bbox[0] << ", " << detection.bbox[1]
               << ", " << detection.bbox[2] << ", " << detection.bbox[3]
               << "] | "
               << fmt::format("{:.1f}%", detection.confidence * 100.0f)
               << " |\n";
        }
    }

    md << "\n## Audit Integrity\n\n";
    md << "- Generated by the BawalVision AWS Lambda C++ custom runtime.\n";
    md << "- This document is a machine-generated serverless compliance artifact.\n";
    return md.str();
}

} // namespace bawal::report
