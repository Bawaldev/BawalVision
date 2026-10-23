#include "bawal/vision/pipeline.hpp"

#include "bawal/vision/simd_food_evaluator.hpp"
#include "bawal/ml/ncnn_generator.hpp"
#include <chrono>
#include <thread>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <array>
#include <span>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

namespace {

bool envFlagValue(const char* name, bool default_value) noexcept {
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return default_value;
    }
    const std::string_view value(raw);
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

} // anonymous namespace

bool InspectionPipeline::initialize(std::string_view model_path, int target_width, int target_height) {
    m_width = target_width;
    m_height = target_height;

    const char* env_c = std::getenv("CONF_THRESHOLD");
    if (env_c != nullptr) {
        try {
            m_conf_threshold.store(std::clamp(std::stof(env_c), 0.25f, 0.98f),
                                   std::memory_order_relaxed);
        } catch (...) {}
    } else {
        m_conf_threshold.store(0.35f, std::memory_order_relaxed);
    }

    // Parse all runtime flags exactly once, before any worker thread starts.
    m_enable_enhance = true;
    if (const char* ee = std::getenv("ENABLE_ENHANCE")) {
        const std::string_view v(ee);
        m_enable_enhance = !(v == "0" || v == "false" || v == "passthrough");
    }
    m_vflip = envFlagValue("CAMERA_VFLIP", false);
    m_hflip = envFlagValue("CAMERA_HFLIP", false);
    m_enable_dewarp = envFlagValue("ENABLE_DEWARP", false);
    m_enable_sr = true;
    if (const char* es = std::getenv("ENABLE_SR")) {
        const std::string_view v(es);
        m_enable_sr = !(v == "0" || v == "false");
    }
    m_last_sr_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    auto calib = CameraCalibrationParams::createDefaultOV5640(cv::Size(m_width, m_height));
    if (!m_dewarper.initCameraCalibration(calib.camera_matrix, calib.dist_coeffs, calib.image_size)) {
        std::cerr << "[Pipeline] Warning: Camera calibration initialization failed.\n";
    }

    // Thread count for YOLO: Limit background inference threads (2-4) to prevent CPU starvation on stream thread
    const int num_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 2, 4);
    if (!model_path.empty()) {
        if (m_classifier.init(model_path, num_threads)) {
            std::cout << "[Pipeline] Model loaded: " << model_path
                      << " (Inference threads: " << num_threads << ")\n";
        } else {
            std::cout << "[Pipeline] Specific model path failed. Fallback to default candidates triggered.\n";
            if (!m_classifier.init("", num_threads)) {
                std::cerr << "[Pipeline] Warning: All candidate models failed to initialize.\n";
            }
        }
    } else {
        (void)m_classifier.init("", num_threads);
    }

    // Persistent CLAHE engine. Clip limit 1.5 is a reasonable default
    // for food imagery — high enough to bring out texture, low enough
    // to avoid amplifying sensor noise.
    m_clahe = cv::createCLAHE(1.5, cv::Size(8, 8));
    m_video_source.clahe = m_clahe;
    m_video_source.target_width = m_width;
    m_video_source.target_height = m_height;

    // Locate weight directory across Docker container and any local working directory
    const std::vector<std::filesystem::path> weight_candidates = {
        "/app/weights",
        "weights",
        "../weights",
        "../../weights",
        "../../../weights",
        "services/weights",
        "../services/weights"
    };
    std::filesystem::path weight_dir = "weights";
    for (const auto& cand : weight_candidates) {
        if (std::filesystem::exists(cand) && std::filesystem::is_directory(cand)) {
            if (std::filesystem::exists(cand / "realesr-animevideov3-x2.param") ||
                std::filesystem::exists(cand / "ai1_vision_int8.onnx")) {
                weight_dir = cand;
                break;
            }
        }
    }

    // Model selection: use the genuine pre-trained realesr-animevideov3-x2 (2x) NCNN model
    // by default for fast, artifact-free CPU super-resolution. Override via REALESRGAN_MODEL.
    std::string model_name = "realesr-animevideov3-x2";
    int model_scale = 2;

    const char* env_model = std::getenv("REALESRGAN_MODEL");
    if (env_model && std::string(env_model).length() > 0) {
        model_name = env_model;
    }
    if (model_name.find("x2") != std::string::npos) model_scale = 2;
    else if (model_name.find("x3") != std::string::npos) model_scale = 3;
    else if (model_name.find("x4") != std::string::npos) model_scale = 4;

    const std::filesystem::path param_file = weight_dir / (model_name + ".param");
    const std::filesystem::path bin_file = weight_dir / (model_name + ".bin");

    if (!std::filesystem::exists(param_file) || !std::filesystem::exists(bin_file)) {
        std::cout << "[Pipeline] Model weights not found locally. Generating via NCNNGenerator: "
                  << model_name << "\n";
        bawal::ml::NCNNModelConfig gen_cfg;
        gen_cfg.model_name = model_name;
        gen_cfg.scale = model_scale;
        gen_cfg.output_dir = weight_dir;
        gen_cfg.sync_dirs = {"/app/weights", "services/weights", "weights"};
        (void)bawal::ml::NCNNGenerator::generate(gen_cfg);
    }

    // RealESRGAN engine configuration.
    //
    // CPU-only settings because the target deployment environment is
    // AWS Lambda, which has no GPU:
    //   - use_gpu = 0          : disable Vulkan entirely
    //   - use_fp16 = 0         : FP16 gives no speedup on CPU
    //   - tile_size = 128      : keeps working set inside L2 cache
    //   - prepadding = 8       : small overlap, still hides tile seams
    RealESRGANConfig enhancer_cfg;
    enhancer_cfg.model_dir = weight_dir;
    enhancer_cfg.model_name = model_name;
    enhancer_cfg.params.scale = model_scale;
    enhancer_cfg.params.tile_size = 128;
    enhancer_cfg.params.prepadding = 8;
    enhancer_cfg.params.use_gpu = 0;
    enhancer_cfg.params.use_fp16 = 0;

    if (m_enhancer.init(enhancer_cfg)) {
        std::cout << "[Pipeline] RealESRGAN engine initialized: "
                  << model_name << " (" << m_enhancer.params.scale << "x, CPU-only)\n";
    }

    return true;
}

bool InspectionPipeline::openVideoSource(SourceMode mode, const std::string& uri) {
    m_video_source.target_width = m_width;
    m_video_source.target_height = m_height;
    m_video_source.clahe = m_clahe;
    return m_video_source.open(mode, uri);
}

bool InspectionPipeline::processNextSourceFrame(const std::vector<DetectionResult>& active_detections,
                                                InspectionOutput& out_inspection,
                                                bool is_mock_video) {
    cv::Mat frame;
    if (!m_video_source.grabNextFrame(frame) || frame.empty()) {
        return false;
    }
    if (m_video_source.didRewind()) {
        resetTracker();
    }
    const bool mock_flag = is_mock_video || (m_video_source.mode == SourceMode::LOCAL_MP4);
    return processStream(frame, active_detections, out_inspection, mock_flag);
}

bool InspectionPipeline::execute(const uint8_t* raw_bytes, size_t len, InspectionOutput& out_inspection) {
    if (!raw_bytes || len == 0) {
        return false;
    }

    DecodedFrame decoded;
    if (!m_receiver.decodeMJPEG(raw_bytes, len, decoded) || decoded.image.empty()) {
        return false;
    }

    out_inspection.frame_id = decoded.frame_id;
    return execute(decoded.image, out_inspection);
}

bool InspectionPipeline::execute(const std::vector<uint8_t>& raw_frame_bytes, InspectionOutput& out_inspection) {
    return execute(raw_frame_bytes.data(), raw_frame_bytes.size(), out_inspection);
}

void InspectionPipeline::enhanceImage(const cv::Mat& src, cv::Mat& dst) {
    if (src.empty()) { dst = src; return; }

    if (!m_enable_enhance) {
        dst = src;
        return;
    }

    // 1. Gray-World AWB
    cv::Scalar mean_bgr = cv::mean(src);
    const double gray_target = (mean_bgr[0] + mean_bgr[1] + mean_bgr[2]) / 3.0;
    const float gain_b = static_cast<float>((mean_bgr[0] > 1.0) ? (gray_target / mean_bgr[0]) : 1.0f);
    const float gain_g = static_cast<float>((mean_bgr[1] > 1.0) ? (gray_target / mean_bgr[1]) : 1.0f);
    const float gain_r = static_cast<float>((mean_bgr[2] > 1.0) ? (gray_target / mean_bgr[2]) : 1.0f);

    // Reuse per-thread work buffers. A thread-local CLAHE instance also avoids
    // sharing its mutable histogram state if callers run pipelines in parallel.
    thread_local IspScratch scratch;
    cv::split(src, scratch.bgr_planes);
    scratch.bgr_planes[0].convertTo(scratch.bgr_planes[0], -1, gain_b, 0);
    scratch.bgr_planes[1].convertTo(scratch.bgr_planes[1], -1, gain_g, 0);
    scratch.bgr_planes[2].convertTo(scratch.bgr_planes[2], -1, gain_r, 0);
    cv::merge(scratch.bgr_planes, scratch.balanced);

    // 2. CLAHE on the L* channel (lifts dark shadows without blowing out bright ceiling lights)
    cv::cvtColor(scratch.balanced, scratch.lab, cv::COLOR_BGR2Lab);
    cv::split(scratch.lab, scratch.lab_planes);
    scratch.clahe->apply(scratch.lab_planes[0], scratch.lab_planes[0]);
    cv::merge(scratch.lab_planes, scratch.lab);
    cv::cvtColor(scratch.lab, scratch.contrast, cv::COLOR_Lab2BGR);

    // OpenCV dispatches addWeighted to its optimized SIMD implementation
    // (including AVX2 when available), without requiring non-portable compiler flags.
    cv::GaussianBlur(scratch.contrast, scratch.blurred, cv::Size(0, 0), 1.75);
    cv::addWeighted(scratch.contrast, 1.25, scratch.blurred, -0.25, 0, dst);
}

bool InspectionPipeline::processStream(const cv::Mat& frame,
                                       const std::vector<DetectionResult>& active_detections,
                                       InspectionOutput& out_inspection,
                                       bool is_mock_video) {
    if (frame.empty()) return false;

    // Ensure incoming frame dimensions from Mock Video (MP4) or Camera Stream
    // are strictly normalized to (m_width, m_height) before entering Dewarp map and ISP
    cv::Mat normalized_frame;
    if (frame.cols != m_width || frame.rows != m_height) {
        cv::resize(frame, normalized_frame, cv::Size(m_width, m_height), 0, 0, cv::INTER_AREA);
    } else {
        normalized_frame = frame;
    }

    const int64_t t_start = cv::getTickCount();
    out_inspection.is_valid = false;

    // Orientation is locked in the sensor driver (set_vflip/set_hmirror).
    cv::Mat oriented = normalized_frame;

    // Lens dewarping: Only active if explicitly requested (ENABLE_DEWARP=1) or perspective map is calibrated
    cv::Mat dewarped;
    if (!is_mock_video && (m_enable_dewarp || m_dewarper.hasPerspective())) {
        if (!m_dewarper.process(oriented, dewarped)) dewarped = oriented;
    } else {
        dewarped = oriented;
    }
    out_inspection.raw_image = dewarped;

    // OpenCV mean-based low-light & underexposure evaluation
    out_inspection.dark_status = m_dark_detector.detect(dewarped);

    // Stream thread: no ISP/SR/CLAHE. AI worker owns all heavy lifting.
    out_inspection.dewarped_image = dewarped;
    out_inspection.detections = active_detections;
    out_inspection.scene_optical = m_classifier.getLastOpticalMetrics();
    // Publish the partial latency before rendering so the HUD bar can display
    // the current frame's processing time.
    out_inspection.processing_time_ms =
        ((cv::getTickCount() - t_start) * 1000.0) / cv::getTickFrequency();
    renderOverlay(out_inspection, out_inspection.annotated_image);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;
    return true;
}

bool InspectionPipeline::runInference(const cv::Mat& enhanced_frame,
                                      std::vector<DetectionResult>& out_detections) {
    if (enhanced_frame.empty()) return false;
    out_detections.clear();

    std::array<DetectionResult, 32> local_buf;
    size_t count = 0;
    if (!m_classifier.detect(enhanced_frame, local_buf, count, m_conf_threshold, 0.40f)) {
        return false;
    }

    out_detections.assign(local_buf.begin(), local_buf.begin() + count);

    // RealESRGAN gating.
    // The classifier already scores spoilage on the native-resolution crop. Super-resolution only adds
    // texture information for SMALL crops (distant / tiny food items). Running a 2x tiled CPU SR pass on
    // large crops cost ~150-900 ms per inference (YOLO itself is ~40-80 ms) and starved the stream thread.
    //  - Only crops whose longest side <= kMaxSrSide (single ncnn tile) are upscaled.
    //  - At most one SR pass per inference, and at most once every kSrMinIntervalMs.
    //  - Disable entirely with ENABLE_SR=0.
    constexpr int kMaxSrSide = 160;
    constexpr int64_t kSrMinIntervalMs = 1000;

    // Reserve the single SR budget slot under a very short critical section,
    // then release the mutex before the expensive inference. Holding it across
    // process() would serialize concurrent callers for hundreds of milliseconds.
    bool sr_budget_available = false;
    {
        std::lock_guard<std::mutex> sr_lock(m_sr_mutex);
        const auto now = std::chrono::steady_clock::now();
        sr_budget_available = m_enable_sr &&
            (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_sr_time).count() >= kSrMinIntervalMs);
        if (sr_budget_available) {
            m_last_sr_time = now;
        }
    }

    for (auto& det : out_detections) {
        if (!sr_budget_available) break;
        if (det.class_id < 0 || det.class_name == "no_food") continue;

        cv::Rect crop_rect = det.bbox & cv::Rect(0, 0, enhanced_frame.cols, enhanced_frame.rows);
        if (crop_rect.width >= 16 && crop_rect.height >= 16 &&
            std::max(crop_rect.width, crop_rect.height) <= kMaxSrSide) {
            cv::Mat food_crop = enhanced_frame(crop_rect);
            cv::Mat sr_crop;

            // RealESRGAN Super-Resolution evaluates fine surface texture for spoilage without destructive downsample blending
            if (m_enhancer.process(food_crop, sr_crop) && !sr_crop.empty()) {
                SpoilageMetric sr_spoilage = evaluateSpoilageAVX2(sr_crop, det.class_name);

                det.spoilage.avg_luminance = 0.5f * sr_spoilage.avg_luminance + 0.5f * det.spoilage.avg_luminance;
                det.spoilage.discoloration_ratio = 0.5f * sr_spoilage.discoloration_ratio + 0.5f * det.spoilage.discoloration_ratio;
                det.spoilage.is_visually_spoiled = (det.spoilage.discoloration_ratio > 0.35f);
            }
            sr_budget_available = false;
        }
    }
    return true;
}

bool InspectionPipeline::execute(const cv::Mat& frame, InspectionOutput& out_inspection) {
    if (frame.empty()) return false;

    // Ensure incoming frame dimensions from Mock Video (MP4) or Camera Stream
    // are strictly normalized to (m_width, m_height) before entering Dewarp map and ISP
    cv::Mat normalized_frame;
    if (frame.cols != m_width || frame.rows != m_height) {
        cv::resize(frame, normalized_frame, cv::Size(m_width, m_height), 0, 0, cv::INTER_AREA);
    } else {
        normalized_frame = frame;
    }

    const int64_t t_start = cv::getTickCount();

    // Orientation is locked in the sensor driver (set_vflip/set_hmirror).
    cv::Mat oriented = normalized_frame;

    // Lens dewarping
    cv::Mat dewarped;
    if (!m_dewarper.process(oriented, dewarped)) dewarped = oriented;
    out_inspection.raw_image = dewarped;

    // OpenCV mean-based low-light & underexposure evaluation
    out_inspection.dark_status = m_dark_detector.detect(dewarped);

    // Lightweight SIMD ISP (zero-copy passthrough for well-lit scenes).
    cv::Mat enhanced;
    enhanceImage(dewarped, enhanced);
    out_inspection.dewarped_image = enhanced;

    std::vector<DetectionResult> detected;
    if (runInference(out_inspection.dewarped_image, detected)) {
        out_inspection.detections = detected;
    } else {
        out_inspection.detections.clear();
    }
    out_inspection.scene_optical = m_classifier.getLastOpticalMetrics();

    // Publish the partial latency before rendering so the HUD bar can display
    // the current frame's processing time.
    out_inspection.processing_time_ms =
        ((cv::getTickCount() - t_start) * 1000.0) / cv::getTickFrequency();

    // Single-pass render overlay at the very end (zero duplicate clone)
    renderOverlay(out_inspection, out_inspection.annotated_image);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;
    return true;
}

void InspectionPipeline::renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display) {
    if (inspection.dewarped_image.empty()) {
        return;
    }

    const float conf_threshold = m_conf_threshold.load(std::memory_order_relaxed);

    // Reuse one pre-allocated HUD buffer instead of cloning a full frame each time.
    if (m_overlay_buffer.size() != inspection.dewarped_image.size() ||
        m_overlay_buffer.type() != inspection.dewarped_image.type()) {
        m_overlay_buffer.create(inspection.dewarped_image.size(), inspection.dewarped_image.type());
    }
    inspection.dewarped_image.copyTo(m_overlay_buffer);
    out_display = m_overlay_buffer;

    // Active detections are the ones that will actually be drawn.
    std::size_t active_detections = 0;
    for (const auto& det : inspection.detections) {
        if (det.class_id >= 0 && det.confidence >= conf_threshold &&
            det.class_name != "no_food" && det.class_name != "no_model") {
            ++active_detections;
        }
    }

    // Top HUD status bar: frame id, latency, and active detections.
    {
        char hud[128];
        std::snprintf(hud, sizeof(hud), "FRAME #%u | %.1f ms | %zu DETECTIONS",
                      inspection.frame_id,
                      inspection.processing_time_ms,
                      active_detections);
        cv::rectangle(out_display, cv::Rect(0, 0, out_display.cols, 26),
                      cv::Scalar(24, 24, 24), cv::FILLED);
        cv::putText(out_display, hud, cv::Point(10, 18),
                    cv::FONT_HERSHEY_SIMPLEX, 0.46, cv::Scalar(235, 235, 235),
                    1, cv::LINE_AA);
    }

    // Low-light underexposure or backlight hotspot conflict banner
    if (inspection.dark_status.is_dark) {
        char dark_txt[96];
        std::snprintf(dark_txt, sizeof(dark_txt), "LOW LIGHT [OpenCV] Gain: %.1fx",
                      static_cast<double>(inspection.dark_status.suggested_boost_gain));
        int base = 0;
        cv::Size tsz = cv::getTextSize(dark_txt, cv::FONT_HERSHEY_SIMPLEX, 0.40, 1, &base);
        cv::Point bg_tl(8, out_display.rows - tsz.height - 12);
        cv::Point bg_br(8 + tsz.width + 8, out_display.rows - 4);
        cv::rectangle(out_display, bg_tl, bg_br, cv::Scalar(20, 20, 180), cv::FILLED);
        cv::putText(out_display, dark_txt, cv::Point(12, out_display.rows - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    } else if (inspection.dark_status.has_hotspot_conflict) {
        static constexpr char kGlareTxt[] = "BACKLIGHT CONFLICT (Gain 2.5x Suppressed)";
        int base = 0;
        cv::Size tsz = cv::getTextSize(kGlareTxt, cv::FONT_HERSHEY_SIMPLEX, 0.40, 1, &base);
        cv::Point bg_tl(8, out_display.rows - tsz.height - 12);
        cv::Point bg_br(8 + tsz.width + 8, out_display.rows - 4);
        cv::rectangle(out_display, bg_tl, bg_br, cv::Scalar(20, 140, 200), cv::FILLED);
        cv::putText(out_display, kGlareTxt, cv::Point(12, out_display.rows - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }

    for (const auto& det : inspection.detections) {
        if (det.class_id < 0 || det.confidence < conf_threshold ||
            det.class_name == "no_food" || det.class_name == "no_model") {
            continue;
        }

        const float spoilage_pct = std::clamp(det.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
        const float freshness_score = std::clamp(100.0f - spoilage_pct, 0.0f, 100.0f);

        const char* status_badge = "PASS";
        cv::Scalar box_color = cv::Scalar(40, 200, 40);

        // Quality Status Determination:
        // REJECT is strictly reserved for confirmed spoilage / degradation.
        // Detections with mild discoloration or near threshold trigger human inspection REVIEW.
        if (det.spoilage.is_visually_spoiled || spoilage_pct >= 45.0f) {
            status_badge = "REJECT";
            box_color = cv::Scalar(30, 30, 235);
        } else if (spoilage_pct >= 25.0f || det.confidence < 0.70f) {
            status_badge = "REVIEW";
            box_color = cv::Scalar(0, 165, 255);
        }

        cv::Rect clamped_box = det.bbox & cv::Rect(0, 0, out_display.cols, out_display.rows);
        if (clamped_box.width <= 0 || clamped_box.height <= 0) {
            continue;
        }

        cv::rectangle(out_display, clamped_box, box_color, 2);

        // Stack-only label formatting: no std::ostringstream/std::string per box.
        char label[160];
        std::snprintf(label, sizeof(label), "%.*s %d%% [%s %d%%]",
                      static_cast<int>(det.class_name.size()), det.class_name.data(),
                      static_cast<int>(det.confidence * 100.0f),
                      status_badge,
                      static_cast<int>(freshness_score));

        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.44, 1, &baseline);
        int badge_y = std::max(clamped_box.y - 4, text_size.height + 4);
        cv::Point badge_tl(clamped_box.x, badge_y - text_size.height - 4);
        cv::Point badge_br(clamped_box.x + text_size.width + 8, badge_y + 2);

        cv::rectangle(out_display, badge_tl, badge_br, box_color, cv::FILLED);
        cv::putText(out_display, label, cv::Point(clamped_box.x + 4, badge_y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }
}

} // namespace bawal::vision
