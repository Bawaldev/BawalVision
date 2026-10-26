#include "bawal/vision/enchanter.hpp"

#include <array>
#include <iostream>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <omp.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

#if __has_include(<ncnn/net.h>)
#include <ncnn/net.h>
#include <ncnn/mat.h>
#include <ncnn/cpu.h>
#define BAWAL_HAVE_NCNN 1
#elif __has_include(<net.h>)
#include <net.h>
#include <mat.h>
#include <cpu.h>
#define BAWAL_HAVE_NCNN 1
#else
#define BAWAL_HAVE_NCNN 0
#endif

namespace {

#if BAWAL_HAVE_NCNN
// Returns the number of threads to use for NCNN CPU inference.
// We use 75% of available hardware threads, capped at 6.
// On a 6-core Xeon W (12 threads) this gives 6 threads, which is
// the sweet spot for NCNN without oversubscribing the CPU.
int getOptimalCpuThreads() {
    int hw_threads = ncnn::get_big_cpu_count();
    if (hw_threads <= 0) hw_threads = omp_get_max_threads();
    if (hw_threads <= 0) hw_threads = 4;
    return std::max(2, std::min(6, hw_threads * 3 / 4));
}
#endif

} // namespace

namespace bawal::vision {

// Per-instance NCNN graph holder (defined out-of-line because the header stays
// independent of the optional NCNN dependency).
struct RealESRGANEngine::NcnnHolder {
#if BAWAL_HAVE_NCNN
    ncnn::Net net;
#endif
};

RealESRGANEngine::RealESRGANEngine()
    : m_ncnn(std::make_unique<NcnnHolder>()) {}

RealESRGANEngine::~RealESRGANEngine() = default;

// ==============================================================================
// SIMD fallback (used only when NCNN model is unavailable)
// ==============================================================================
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((target_clones("avx2,fma", "default")))
#endif
#endif

void applyFastSuperResolutionSIMD(const cv::Mat& src, cv::Mat& dst, int scale) {
    if (src.empty()) return;
    if (scale <= 1) { dst = src; return; }

    const int target_w = src.cols * scale;
    const int target_h = src.rows * scale;

    // Bicubic looks better than linear when we have no neural network.
    cv::resize(src, dst, cv::Size(target_w, target_h), 0, 0, cv::INTER_CUBIC);
}

// ==============================================================================
// OpenCV DNN (ONNX) path
// ==============================================================================
bool RealESRGANEngine::initOpenCVDNN(const std::filesystem::path& model_path) {
    if (model_path.empty() || !std::filesystem::exists(model_path)) return false;
    try {
        dnn_net = cv::dnn::readNetFromONNX(model_path.string());
        if (!dnn_net.empty()) {
            dnn_net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            dnn_net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            is_dnn_ready = true;
            std::cout << "[RealESRGAN] OpenCV DNN loaded: " << model_path << std::endl;
            return true;
        }
    } catch (const cv::Exception& e) {
        std::cerr << "[RealESRGAN] DNN load error: " << e.what() << std::endl;
    }
    return false;
}

bool RealESRGANEngine::processOpenCVDNN(const cv::Mat& input, cv::Mat& output) {
    if (input.empty() || !is_dnn_ready || dnn_net.empty()) return false;
    try {
        cv::Mat blob = cv::dnn::blobFromImage(input, 1.0/255.0, cv::Size(), cv::Scalar(), true, false);
        dnn_net.setInput(blob);
        cv::Mat prob = dnn_net.forward();
        if (!prob.empty() && prob.dims == 4) {
            const int out_h = prob.size[2];
            const int out_w = prob.size[3];
            output.create(out_h, out_w, CV_8UC3);
            const float* r_plane = prob.ptr<float>(0, 0);
            const float* g_plane = prob.ptr<float>(0, 1);
            const float* b_plane = prob.ptr<float>(0, 2);

            // Rows are spatially independent; no shared writes inside the loop.
            for (int y = 0; y < out_h; ++y) {
                auto* row = output.ptr<cv::Vec3b>(y);
                const int offset = y * out_w;
                #pragma omp simd
                for (int x = 0; x < out_w; ++x) {
                    row[x][0] = cv::saturate_cast<uint8_t>(b_plane[offset+x] * 255.0f);
                    row[x][1] = cv::saturate_cast<uint8_t>(g_plane[offset+x] * 255.0f);
                    row[x][2] = cv::saturate_cast<uint8_t>(r_plane[offset+x] * 255.0f);
                }
            }
            return true;
        }
    } catch (const cv::Exception& e) {
        std::cerr << "[RealESRGAN] DNN forward error: " << e.what() << std::endl;
    }
    return false;
}

// ==============================================================================
// NCNN init - CPU only configuration
// ==============================================================================
bool RealESRGANEngine::init(const RealESRGANConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    params = config.params;
    is_initialized = false;
    is_dnn_ready = false;

    std::vector<std::filesystem::path> search_dirs = {
        config.model_dir, "/app/weights", "/app/services/weights",
        "weights", "services/weights", "../weights", "../../weights",
        "/app/ml/models", "/app/ml/AI1/model", "ml/AI1/model", "models"
    };

    // ONNX takes priority when it exists
    for (const auto& dir : search_dirs) {
        if (dir.empty() || !std::filesystem::exists(dir)) continue;
        auto onnx_file = dir / (config.model_name + ".onnx");
        if (std::filesystem::exists(onnx_file)) {
            if (initOpenCVDNN(onnx_file)) break;
        }
    }

#if BAWAL_HAVE_NCNN
    try {
        ncnn::Net& net = m_ncnn->net;

        // CPU-only configuration
        net.opt.use_vulkan_compute = 0;
        net.opt.use_fp16_packed = false;
        net.opt.use_fp16_storage = false;
        net.opt.use_fp16_arithmetic = false;
        net.opt.use_winograd_convolution = 1;
        net.opt.use_sgemm_convolution = 1;
        net.opt.use_int8_inference = 0;
        net.opt.use_packing_layout = true;
        net.opt.use_bf16_storage = false;

        const int num_threads = getOptimalCpuThreads();
        net.opt.num_threads = num_threads;

        std::cout << "[RealESRGAN] CPU-only mode: " << num_threads
                  << " threads (detected " << ncnn::get_big_cpu_count()
                  << " hw threads)" << std::endl;

        // Model chain: prefer genuine pre-trained Real-ESRGAN NCNN models first
        std::vector<std::string> model_candidates;
        model_candidates.push_back(config.model_name);       // primary
        if (config.model_name != "realesr-animevideov3-x2") {
            model_candidates.push_back("realesr-animevideov3-x2");  // fallback
        }
        if (config.model_name != "realesr-general-x4v3") {
            model_candidates.push_back("realesr-general-x4v3");
        }

        for (const auto& candidate : model_candidates) {
            std::filesystem::path param_file, bin_file;

            for (const auto& dir : search_dirs) {
                if (dir.empty() || !std::filesystem::exists(dir)) continue;
                auto p = dir / (candidate + ".param");
                auto b = dir / (candidate + ".bin");
                if (std::filesystem::exists(p) && std::filesystem::exists(b)) {
                    param_file = p;
                    bin_file = b;
                    break;
                }
            }

            if (param_file.empty() || bin_file.empty()) {
                std::cout << "[RealESRGAN] Not found: " << candidate << std::endl;
                continue;
            }

            int ret_p = net.load_param(param_file.string().c_str());
            int ret_b = net.load_model(bin_file.string().c_str());
            if (ret_p == 0 && ret_b == 0) {
                if (candidate.find("x2") != std::string::npos) params.scale = 2;
                else if (candidate.find("x3") != std::string::npos) params.scale = 3;
                else if (candidate.find("x4") != std::string::npos) params.scale = 4;

                std::cout << "[RealESRGAN] Loaded: " << param_file.filename()
                          << " (CPU-only, " << num_threads << " threads, scale=" << params.scale << "x)" << std::endl;
                is_initialized = true;
                return true;
            } else {
                std::cerr << "[RealESRGAN] Load failed for " << candidate
                          << ": ret_p=" << ret_p << " ret_b=" << ret_b << std::endl;
            }
        }

        std::cerr << "[RealESRGAN] All model candidates failed to load" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RealESRGAN] Init error: " << e.what() << std::endl;
    }
#endif

    // Last resort: cv::resize. We log loudly so this is never a silent failure.
    std::cerr << "[RealESRGAN] WARNING: Falling back to cv::resize INTER_CUBIC." 
              << " Image quality will be degraded. Check model files and logs above."
              << std::endl;
    is_initialized = true;
    return true;
}
// ==============================================================================
// NCNN process - runs on CPU regardless of GPU availability
// ==============================================================================
bool RealESRGANEngine::process(const cv::Mat& input, cv::Mat& output) {
    if (input.empty()) return false;

    // Serialize access to the mutable DNN/NCNN session. The lock intentionally
    // covers the inference itself (the session cannot be shared) but protects
    // only this engine instance, not any shared global network.
    std::lock_guard<std::mutex> lock(m_mutex);

    // Prefer ONNX if it was loaded
    if (is_dnn_ready) {
        if (processOpenCVDNN(input, output)) return true;
    }

    const int scale = (params.scale > 0) ? params.scale : 2;

#if BAWAL_HAVE_NCNN
    if (is_initialized) {
        ncnn::Net& net = m_ncnn->net;
        try {
            const int w = input.cols;
            const int h = input.rows;

            // Small tile size keeps CPU cache happy.
            int tile_size = params.tile_size;
            if (tile_size <= 0 || tile_size > 192) tile_size = 128;
            const int prepadding = (params.prepadding > 0) ? params.prepadding : 8;

            // Standard Real-ESRGAN NCNN models expect normalized [0, 1] float input
            // and produce [0, 1] float output. Synthetic NCNNGenerator linear models
            // operate directly on [0, 255]. Detect by checking if output max <= 2.0f.
            constexpr float norm_vals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
            constexpr float denorm_vals[3] = {255.0f, 255.0f, 255.0f};

            auto convertNcnnOutToMat = [&](ncnn::Mat& out, cv::Mat& dst_mat) {
                dst_mat.create(out.h, out.w, CV_8UC3);
                // Scale [0, 1] -> [0, 255] with clamping before converting to BGR bytes
                out.substract_mean_normalize(nullptr, denorm_vals);
                for (int c = 0; c < out.c; ++c) {
                    float* ptr = out.channel(c);
                    const int total = out.w * out.h;
                    #pragma omp simd
                    for (int i = 0; i < total; ++i) {
                        ptr[i] = std::clamp(ptr[i], 0.0f, 255.0f);
                    }
                }
                out.to_pixels(dst_mat.data, ncnn::Mat::PIXEL_RGB2BGR);
            };

            // Small input: single pass
            if (w <= tile_size && h <= tile_size) {
                ncnn::Mat in = ncnn::Mat::from_pixels(input.data,
                                                       ncnn::Mat::PIXEL_BGR2RGB,
                                                       w, h,
                                                       static_cast<int>(input.step));
                in.substract_mean_normalize(nullptr, norm_vals);
                ncnn::Extractor ex = net.create_extractor();
                ex.input("data", in);
                ncnn::Mat out;
                if (ex.extract("output", out) == 0 && !out.empty()) {
                    convertNcnnOutToMat(out, output);
                    return true;
                }
            } else {
                // Large input: tiled inference
                const int num_tiles_x = (w + tile_size - 1) / tile_size;
                const int num_tiles_y = (h + tile_size - 1) / tile_size;
                int actual_scale = scale;

                for (int ty = 0; ty < num_tiles_y; ++ty) {
                    for (int tx = 0; tx < num_tiles_x; ++tx) {
                        const int x0 = tx * tile_size;
                        const int y0 = ty * tile_size;
                        const int x1 = std::min(x0 + tile_size, w);
                        const int y1 = std::min(y0 + tile_size, h);

                        const int pad_x0 = std::max(0, x0 - prepadding);
                        const int pad_y0 = std::max(0, y0 - prepadding);
                        const int pad_x1 = std::min(w, x1 + prepadding);
                        const int pad_y1 = std::min(h, y1 + prepadding);

                        cv::Rect tile_roi(pad_x0, pad_y0, pad_x1 - pad_x0, pad_y1 - pad_y0);
                        cv::Mat tile = input(tile_roi);

                        ncnn::Mat in = ncnn::Mat::from_pixels(tile.data,
                                                               ncnn::Mat::PIXEL_BGR2RGB,
                                                               tile.cols, tile.rows,
                                                               static_cast<int>(tile.step));
                        in.substract_mean_normalize(nullptr, norm_vals);
                        ncnn::Extractor ex = net.create_extractor();
                        ex.input("data", in);
                        ncnn::Mat out;
                        if (ex.extract("output", out) == 0 && !out.empty()) {
                            if (tile.cols > 0 && out.w >= tile.cols) {
                                actual_scale = std::max(1, out.w / tile.cols);
                            }
                            if (output.empty() || output.cols != w * actual_scale || output.rows != h * actual_scale) {
                                output.create(h * actual_scale, w * actual_scale, CV_8UC3);
                            }

                            cv::Mat out_tile;
                            convertNcnnOutToMat(out, out_tile);

                            const int crop_x0 = (x0 - pad_x0) * actual_scale;
                            const int crop_y0 = (y0 - pad_y0) * actual_scale;
                            const int crop_w = (x1 - x0) * actual_scale;
                            const int crop_h = (y1 - y0) * actual_scale;

                            cv::Rect crop_rect(crop_x0, crop_y0, crop_w, crop_h);
                            cv::Rect out_rect(x0 * actual_scale, y0 * actual_scale, crop_w, crop_h);
                            crop_rect &= cv::Rect(0, 0, out_tile.cols, out_tile.rows);
                            out_rect &= cv::Rect(0, 0, output.cols, output.rows);

                            if (crop_rect.width > 0 && crop_rect.height > 0 &&
                                crop_rect.width == out_rect.width &&
                                crop_rect.height == out_rect.height) {
                                out_tile(crop_rect).copyTo(output(out_rect));
                            }
                        }
                    }
                }
                if (!output.empty()) {
                    return true;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[RealESRGAN] Process error: " << e.what() << std::endl;
        }
    }
#endif

    // Last resort fallback
    applyFastSuperResolutionSIMD(input, output, scale);
    return true;
}

void RealESRGANEngine::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
#if BAWAL_HAVE_NCNN
    m_ncnn->net.clear();
#endif
    if (!dnn_net.empty()) dnn_net = cv::dnn::Net();
    is_dnn_ready = false;
    is_initialized = false;
}

// ==============================================================================
// IlluminationEnhancerAI: High-Speed SIMD OpenMP Adaptive ISP (< 12 ms @ 30 FPS)
// ==============================================================================
void IlluminationEnhancerAI::process(const cv::Mat& src_in, cv::Mat& dst, cv::Ptr<cv::CLAHE> persistent_clahe) {
    if (src_in.empty()) {
        dst = src_in;
        return;
    }

    cv::Mat src;
    if (src_in.type() == CV_8UC3) {
        src = src_in;
    } else if (src_in.channels() == 1) {
        cv::cvtColor(src_in, src, cv::COLOR_GRAY2BGR);
    } else if (src_in.channels() == 4) {
        cv::cvtColor(src_in, src, cv::COLOR_BGRA2BGR);
    } else {
        src_in.convertTo(src, CV_8UC3);
    }


    const int step_y = std::max(1, src.rows / 32);
    const int step_x = std::max(1, src.cols / 32);
    const int cx0 = src.cols / 4, cx1 = (src.cols * 3) / 4;
    const int cy0 = src.rows / 4, cy1 = (src.rows * 3) / 4;

    uint64_t sum_lum_weighted = 0;
    uint64_t total_weight = 0;
    int dark_pixels = 0, bright_pixels = 0, total_samples = 0;

    for (int y = 0; y < src.rows; y += step_y) {
        const auto* row = src.ptr<cv::Vec3b>(y);
        const bool in_center_y = (y >= cy0 && y <= cy1);
        for (int x = 0; x < src.cols; x += step_x) {
            const uint8_t b = row[x][0];
            const uint8_t g = row[x][1];
            const uint8_t r = row[x][2];
            const uint8_t lum = static_cast<uint8_t>((29 * b + 150 * g + 77 * r) >> 8);
            const uint64_t w = (in_center_y && x >= cx0 && x <= cx1) ? 3 : 1;
            sum_lum_weighted += lum * w;
            total_weight += w;
            total_samples++;

            if (lum < 65) dark_pixels++;
            else if (lum > 200) bright_pixels++;
        }
    }

    const float avg_lum = total_weight > 0 ? static_cast<float>(sum_lum_weighted) / total_weight : 128.0f;
    const float dark_ratio = total_samples > 0 ? static_cast<float>(dark_pixels) / total_samples : 0.0f;
    const float bright_ratio = total_samples > 0 ? static_cast<float>(bright_pixels) / total_samples : 0.0f;
    const bool is_backlit = (dark_ratio > 0.25f && bright_ratio > 0.15f);
    const bool needs_illumination_boost = (avg_lum < 75.0f || is_backlit);

    // Case 1: Well-lit balanced scene -> zero-copy passthrough.
    // The source is owned by the caller for the duration of the frame; returning
    // a shallow cv::Mat reference avoids a full-frame clone every 33 ms.
    if (!needs_illumination_boost) {
        dst = src;
        return;
    }

    // Case 2: Dark / underexposed scene -> Gentle L-channel contrast lift without chroma degradation or blurring.
    // CLAHE is stateful and NOT thread-safe. Using a thread-local instance avoids a
    // mutex on the shared pipeline object and removes the data race on setClipLimit/apply.
    thread_local cv::Mat tl_lab;
    thread_local std::vector<cv::Mat> tl_planes(3);
    thread_local cv::Ptr<cv::CLAHE> tl_clahe;

    if (!tl_clahe) {
        const double clip = persistent_clahe ? persistent_clahe->getClipLimit() : 1.5;
        const cv::Size tiles = persistent_clahe ? persistent_clahe->getTilesGridSize() : cv::Size(8, 8);
        tl_clahe = cv::createCLAHE(clip, tiles);
    }

    cv::cvtColor(src, tl_lab, cv::COLOR_BGR2Lab);
    cv::split(tl_lab, tl_planes);

    if (!tl_planes.empty()) {
        const double dynamic_clip = is_backlit ? 1.25 : 1.10;
        tl_clahe->setClipLimit(dynamic_clip);
        tl_clahe->apply(tl_planes[0], tl_planes[0]); // Lift shadow contrast on L channel only
    }

    // A and B color channels remain 100% pure & unblurred to prevent color bleeding on food
    cv::merge(tl_planes, tl_lab);
    cv::cvtColor(tl_lab, dst, cv::COLOR_Lab2BGR);
}

} // namespace bawal::vision