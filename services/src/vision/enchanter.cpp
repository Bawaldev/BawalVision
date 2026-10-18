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

namespace bawal::vision {

namespace {

#if BAWAL_HAVE_NCNN
static std::unique_ptr<ncnn::Net> s_realesrgan_net = nullptr;
#endif

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

} // anonymous namespace

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
    if (scale <= 1) { dst = src.clone(); return; }

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

            #pragma omp parallel for schedule(static)
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
        if (!s_realesrgan_net) {
            s_realesrgan_net = std::make_unique<ncnn::Net>();
        }

        // CPU-only configuration
        s_realesrgan_net->opt.use_vulkan_compute = 0;
        s_realesrgan_net->opt.use_fp16_packed = false;
        s_realesrgan_net->opt.use_fp16_storage = false;
        s_realesrgan_net->opt.use_fp16_arithmetic = false;
        s_realesrgan_net->opt.use_winograd_convolution = 1;
        s_realesrgan_net->opt.use_sgemm_convolution = 1;
        s_realesrgan_net->opt.use_int8_inference = 0;
        s_realesrgan_net->opt.use_packing_layout = true;
        s_realesrgan_net->opt.use_bf16_storage = false;

        const int num_threads = getOptimalCpuThreads();
        s_realesrgan_net->opt.num_threads = num_threads;

        std::cout << "[RealESRGAN] CPU-only mode: " << num_threads
                  << " threads (detected " << ncnn::get_big_cpu_count()
                  << " hw threads)" << std::endl;

        // Model chain: try primary first, then fallback.
        // The fallback is used only when the primary fails to load.
        std::vector<std::string> model_candidates;
        model_candidates.push_back(config.model_name);       // primary
        if (config.model_name != "realesr-animevideov3-x2") {
            model_candidates.push_back("realesr-animevideov3-x2");  // fallback
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

            int ret_p = s_realesrgan_net->load_param(param_file.string().c_str());
            int ret_b = s_realesrgan_net->load_model(bin_file.string().c_str());
            if (ret_p == 0 && ret_b == 0) {
                std::cout << "[RealESRGAN] Loaded: " << param_file.filename()
                          << " (CPU-only, " << num_threads << " threads)" << std::endl;
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

    // Prefer ONNX if it was loaded
    if (is_dnn_ready) {
        if (processOpenCVDNN(input, output)) return true;
    }

    const int scale = (params.scale > 0) ? params.scale : 2;

#if BAWAL_HAVE_NCNN
    if (is_initialized && s_realesrgan_net) {
        try {
            const int w = input.cols;
            const int h = input.rows;

            // Small tile size keeps CPU cache happy.
            int tile_size = params.tile_size;
            if (tile_size <= 0 || tile_size > 192) tile_size = 128;
            const int prepadding = (params.prepadding > 0) ? params.prepadding : 8;

            // Small input: single pass
            if (w <= tile_size && h <= tile_size) {
                ncnn::Mat in = ncnn::Mat::from_pixels(input.data,
                                                       ncnn::Mat::PIXEL_BGR2RGB,
                                                       w, h,
                                                       static_cast<int>(input.step));
                ncnn::Extractor ex = s_realesrgan_net->create_extractor();
                ex.input("data", in);
                ncnn::Mat out;
                if (ex.extract("output", out) == 0 && !out.empty()) {
                    output.create(out.h, out.w, CV_8UC3);
                    out.to_pixels(output.data, ncnn::Mat::PIXEL_RGB2BGR);
                    return true;
                }
            } else {
                // Large input: tiled inference
                output.create(h * scale, w * scale, CV_8UC3);
                const int num_tiles_x = (w + tile_size - 1) / tile_size;
                const int num_tiles_y = (h + tile_size - 1) / tile_size;

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
                        ncnn::Extractor ex = s_realesrgan_net->create_extractor();
                        ex.input("data", in);
                        ncnn::Mat out;
                        if (ex.extract("output", out) == 0 && !out.empty()) {
                            cv::Mat out_tile(out.h, out.w, CV_8UC3);
                            out.to_pixels(out_tile.data, ncnn::Mat::PIXEL_RGB2BGR);

                            const int crop_x0 = (x0 - pad_x0) * scale;
                            const int crop_y0 = (y0 - pad_y0) * scale;
                            const int crop_w = (x1 - x0) * scale;
                            const int crop_h = (y1 - y0) * scale;

                            cv::Rect crop_rect(crop_x0, crop_y0, crop_w, crop_h);
                            cv::Rect out_rect(x0 * scale, y0 * scale, crop_w, crop_h);
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
                return true;
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
#if BAWAL_HAVE_NCNN
    if (s_realesrgan_net) {
        s_realesrgan_net->clear();
        s_realesrgan_net.reset();
    }
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
    double sum_b = 0, sum_g = 0, sum_r = 0;
    int mid_count = 0;

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

            if (lum >= 30 && lum <= 210) {
                sum_b += b; sum_g += g; sum_r += r;
                mid_count++;
            }
        }
    }

    const float avg_lum = total_weight > 0 ? static_cast<float>(sum_lum_weighted) / total_weight : 128.0f;
    const float dark_ratio = total_samples > 0 ? static_cast<float>(dark_pixels) / total_samples : 0.0f;
    const float bright_ratio = total_samples > 0 ? static_cast<float>(bright_pixels) / total_samples : 0.0f;
    const bool is_backlit = (dark_ratio > 0.20f && bright_ratio > 0.10f);
    cv::Mat denoised_src;
    const double spatial_sigma = (avg_lum < 80.0f || is_backlit) ? 0.85 : 0.55;
    cv::GaussianBlur(src, denoised_src, cv::Size(3, 3), spatial_sigma);

    const float shadow_gamma = is_backlit ? 0.62f : std::clamp(std::pow(avg_lum / 135.0f, 0.38f), 0.65f, 0.92f);
    uint8_t tone_lut[256];
    for (int i = 0; i < 256; ++i) {
        float x = static_cast<float>(i) * (1.0f / 255.0f);
        float y = std::pow(x, shadow_gamma);
        if (y > 0.72f) {
            const float excess = y - 0.72f;
            y = 0.72f + (0.28f * excess) / (excess + 0.22f);
        }
        tone_lut[i] = cv::saturate_cast<uint8_t>(y * 255.0f);
    }

    float gain_b = 1.0f, gain_g = 1.0f, gain_r = 1.0f;
    if (mid_count > 32) {
        const double avg_b = sum_b / mid_count;
        const double avg_g = sum_g / mid_count;
        const double avg_r = sum_r / mid_count;
        const double gray_target = (avg_b + avg_g + avg_r) / 3.0;

        gain_b = std::clamp(static_cast<float>(gray_target / std::max(1.0, avg_b)), 0.88f, 1.18f);
        gain_g = std::clamp(static_cast<float>(gray_target / std::max(1.0, avg_g)), 0.92f, 1.08f);
        gain_r = std::clamp(static_cast<float>(gray_target / std::max(1.0, avg_r)), 0.88f, 1.18f);

        const float g_norm = (gain_b + gain_g + gain_r) * (1.0f / 3.0f);
        gain_b /= g_norm; gain_g /= g_norm; gain_r /= g_norm;
    }

    thread_local cv::Mat tl_tone_mapped, tl_lab, tl_sharpened;
    thread_local std::vector<cv::Mat> tl_planes(3);

    tl_tone_mapped.create(denoised_src.size(), CV_8UC3);

    const int tone_rows = denoised_src.rows;
    const int tone_cols = denoised_src.cols;
    const size_t src_step = denoised_src.step;
    const size_t dst_step = tl_tone_mapped.step;
    const uint8_t* src_base = denoised_src.data;
    uint8_t* dst_base = tl_tone_mapped.data;

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < tone_rows; ++y) {
        const auto* __restrict s = reinterpret_cast<const cv::Vec3b*>(src_base + y * src_step);
        auto* __restrict d = reinterpret_cast<cv::Vec3b*>(dst_base + y * dst_step);
        #pragma omp simd
        for (int x = 0; x < tone_cols; ++x) {
            const int max_c = std::max({s[x][0], s[x][1], s[x][2]});
            const float hl_protect = std::clamp((static_cast<float>(max_c) - 175.0f) * (1.0f / 65.0f), 0.0f, 1.0f);
            const float gb = gain_b * (1.0f - hl_protect) + hl_protect;
            const float gg = gain_g * (1.0f - hl_protect) + hl_protect;
            const float gr = gain_r * (1.0f - hl_protect) + hl_protect;

            d[x][0] = cv::saturate_cast<uint8_t>(static_cast<float>(tone_lut[s[x][0]]) * gb);
            d[x][1] = cv::saturate_cast<uint8_t>(static_cast<float>(tone_lut[s[x][1]]) * gg);
            d[x][2] = cv::saturate_cast<uint8_t>(static_cast<float>(tone_lut[s[x][2]]) * gr);
        }
    }

    cv::cvtColor(tl_tone_mapped, tl_lab, cv::COLOR_BGR2Lab);
    cv::split(tl_lab, tl_planes);

    const double dynamic_clip = is_backlit ? 1.85 : ((avg_lum < 70.0f) ? 1.0 : 1.45);
    if (persistent_clahe && !tl_planes.empty()) {
        persistent_clahe->setClipLimit(dynamic_clip);
        persistent_clahe->apply(tl_planes[0], tl_planes[0]); // In-place! Tanpa cv::Mat clahe_l baru
    }

    if (tl_planes.size() >= 3) {
        cv::GaussianBlur(tl_planes[1], tl_planes[1], cv::Size(5, 5), 1.4); // In-place!
        cv::GaussianBlur(tl_planes[2], tl_planes[2], cv::Size(5, 5), 1.4); // In-place!

        const size_t l_step = tl_planes[0].step;
        const size_t a_step = tl_planes[1].step;
        const size_t b_step = tl_planes[2].step;
        const uint8_t* l_base = tl_planes[0].data;
        uint8_t* a_base = tl_planes[1].data;
        uint8_t* b_base = tl_planes[2].data;
        const int lab_rows = tl_lab.rows;
        const int lab_cols = tl_lab.cols;
        constexpr float vibrancy = 1.08f;

        #pragma omp parallel for schedule(static)
        for (int y = 0; y < lab_rows; ++y) {
            const auto* __restrict l_row = l_base + y * l_step;
            auto* __restrict a_row = a_base + y * a_step;
            auto* __restrict b_row = b_base + y * b_step;
            #pragma omp simd
            for (int x = 0; x < lab_cols; ++x) {
                const float l_val = static_cast<float>(l_row[x]);
                float da = static_cast<float>(a_row[x]) - 128.0f;
                float db = static_cast<float>(b_row[x]) - 128.0f;

                const float chroma_sq = da * da + db * db;
                if (chroma_sq < 25.0f) {
                    const float factor = chroma_sq * (1.0f / 25.0f);
                    da *= factor;
                    db *= factor;
                }

                const float shadow_scale = std::clamp((l_val - 15.0f) * (1.0f / 35.0f), 0.0f, 1.0f);
                a_row[x] = cv::saturate_cast<uint8_t>(128.0f + da * shadow_scale * vibrancy);
                b_row[x] = cv::saturate_cast<uint8_t>(128.0f + db * shadow_scale * vibrancy);
            }
        }

        cv::merge(tl_planes, tl_lab);
        cv::cvtColor(tl_lab, tl_sharpened, cv::COLOR_Lab2BGR);
    } else {
        tl_sharpened = tl_tone_mapped;
    }

    const float sharp_strength = (avg_lum < 65.0f) ? 0.08f : 0.15f;
    const float center_weight = 1.0f + 4.0f * sharp_strength;
    const cv::Matx33f kernel(
         0.0f, -sharp_strength, 0.0f,
        -sharp_strength, center_weight, -sharp_strength,
         0.0f, -sharp_strength, 0.0f);
    cv::filter2D(tl_sharpened, dst, -1, kernel);
}

} // namespace bawal::vision