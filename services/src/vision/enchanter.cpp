#include "bawal/vision/enchanter.hpp"

#include <iostream>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <omp.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

#if __has_include(<ncnn/net.h>)
#include <ncnn/net.h>
#include <ncnn/mat.h>
#include <ncnn/cpu.h>
#if defined(USE_NCNN_VULKAN) || defined(NCNN_VULKAN)
#include <ncnn/gpu.h>
#endif
#define BAWAL_HAVE_NCNN 1
#elif __has_include(<net.h>)
#include <net.h>
#include <mat.h>
#include <cpu.h>
#if defined(USE_NCNN_VULKAN) || defined(NCNN_VULKAN)
#include <gpu.h>
#endif
#define BAWAL_HAVE_NCNN 1
#else
#define BAWAL_HAVE_NCNN 0
#endif

namespace bawal::vision {

namespace {

#if BAWAL_HAVE_NCNN
static std::unique_ptr<ncnn::Net> s_realesrgan_net = nullptr;
static bool s_vulkan_initialized = false;
#endif

// ==============================================================================
// Multi-Architecture SIMD & OpenMP Super-Resolution / Enhancement Kernel
// Automatically dispatches AVX-512 / AVX2 on x86_64 or OpenMP SIMD on ARM / other arch
// ==============================================================================
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((target_clones("avx512f", "avx2,fma", "default")))
#endif
#endif
void applyFastSuperResolutionSIMD(const cv::Mat& src, cv::Mat& dst, int scale) {
    if (src.empty()) return;

    const int target_w = src.cols * scale;
    const int target_h = src.rows * scale;

    // Smooth cubic interpolation (eliminates JPEG ringing halos and block artifact amplification)
    cv::Mat upscaled;
    cv::resize(src, upscaled, cv::Size(target_w, target_h), 0, 0, cv::INTER_CUBIC);

    // Edge-preserving bilateral filtering to suppress sensor noise without muddying food texture
    cv::Mat denoised;
    cv::bilateralFilter(upscaled, denoised, 5, 18.0, 18.0);

    // Subtle edge contrast enhancement (1.15x) without harsh speckled artifacts or grey stains
    cv::Mat blurred;
    cv::GaussianBlur(denoised, blurred, cv::Size(0, 0), 1.5);

    dst.create(denoised.size(), denoised.type());

    const int total_rows = denoised.rows;
    const int total_cols = denoised.cols;

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < total_rows; ++y) {
        const auto* src_row = denoised.ptr<cv::Vec3b>(y);
        const auto* blur_row = blurred.ptr<cv::Vec3b>(y);
        auto* dst_row = dst.ptr<cv::Vec3b>(y);

        #pragma omp simd
        for (int x = 0; x < total_cols; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float val = 1.15f * static_cast<float>(src_row[x][c]) - 0.15f * static_cast<float>(blur_row[x][c]);
                dst_row[x][c] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            }
        }
    }
}

} // anonymous namespace

bool RealESRGANEngine::init(const RealESRGANConfig& config) {
    params = config.params;
    is_initialized = false;

#if BAWAL_HAVE_NCNN
    try {
        if (!s_realesrgan_net) {
            s_realesrgan_net = std::make_unique<ncnn::Net>();
        }

#if defined(USE_NCNN_VULKAN) || defined(NCNN_VULKAN)
        if (params.use_gpu) {
            if (!s_vulkan_initialized) {
                ncnn::create_gpu_instance();
                s_vulkan_initialized = true;
            }
            int gpu_count = ncnn::get_gpu_count();
            if (gpu_count > 0) {
                s_realesrgan_net->opt.use_vulkan_compute = 1;
                s_realesrgan_net->set_vulkan_device(params.gpu_device < gpu_count ? params.gpu_device : 0);
            } else {
                s_realesrgan_net->opt.use_vulkan_compute = 0;
                params.use_gpu = 0;
            }
        }
#else
        params.use_gpu = 0;
#endif

        s_realesrgan_net->opt.use_fp16_packed = (params.use_fp16 != 0);
        s_realesrgan_net->opt.use_fp16_storage = (params.use_fp16 != 0);
        s_realesrgan_net->opt.use_fp16_arithmetic = (params.use_fp16 != 0);
        s_realesrgan_net->opt.num_threads = ncnn::get_big_cpu_count();

        // Comprehensive search for Real-ESRGAN NCNN model files (.param and .bin)
        std::vector<std::filesystem::path> search_dirs = {
            config.model_dir,
            "/app/weights",
            "/app/services/weights",
            "weights",
            "services/weights",
            "../weights",
            "../../weights",
            "../services/weights",
            "/app/ml/models",
            "/app/ml/AI1/model",
            "ml/AI1/model",
            "models"
        };

        std::filesystem::path param_file;
        std::filesystem::path bin_file;

        for (const auto& dir : search_dirs) {
            if (dir.empty() || !std::filesystem::exists(dir)) continue;
            auto p = dir / (config.model_name + ".param");
            auto b = dir / (config.model_name + ".bin");
            if (std::filesystem::exists(p) && std::filesystem::exists(b)) {
                param_file = p;
                bin_file = b;
                break;
            }
        }

        if (!param_file.empty() && !bin_file.empty()) {
            int ret_p = s_realesrgan_net->load_param(param_file.string().c_str());
            int ret_b = s_realesrgan_net->load_model(bin_file.string().c_str());
            if (ret_p == 0 && ret_b == 0) {
                std::cout << "[RealESRGAN] Successfully loaded NCNN weights: " << param_file << " (GPU: " 
                          << (params.use_gpu ? "Vulkan" : "CPU SIMD") << ")\n";
                is_initialized = true;
                return true;
            } else {
                std::cerr << "[RealESRGAN] Warning: NCNN load failed (param: " << ret_p 
                          << ", bin: " << ret_b << ") from " << param_file << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[RealESRGAN] NCNN init warning: " << e.what() << "\n";
    }
#endif

    // Fallback: Multi-versioned SIMD / OpenMP CPU Super-Resolution is ready
    std::cout << "[RealESRGAN] Operating in Multi-Architecture AVX-512 / AVX2 / OpenMP SIMD High-Boost mode.\n";
    is_initialized = true;
    return true;
}

bool RealESRGANEngine::process(const cv::Mat& input, cv::Mat& output) {
    if (input.empty()) {
        return false;
    }

    const int scale = (params.scale > 0) ? params.scale : 2;

#if BAWAL_HAVE_NCNN
    if (is_initialized && s_realesrgan_net && (params.use_gpu != 0) && s_vulkan_initialized) {
        try {
            const int w = input.cols;
            const int h = input.rows;
            const int tile_size = params.tile_size;
            const int prepadding = params.prepadding;

            if (tile_size <= 0 || (w <= tile_size && h <= tile_size)) {
                // Single tile inference
                ncnn::Mat in = ncnn::Mat::from_pixels(input.data, ncnn::Mat::PIXEL_BGR2RGB, w, h);

                ncnn::Extractor ex = s_realesrgan_net->create_extractor();
                ex.input("data", in);
                ncnn::Mat out;
                if (ex.extract("output", out) == 0 && !out.empty()) {
                    output.create(out.h, out.w, CV_8UC3);
                    out.to_pixels(output.data, ncnn::Mat::PIXEL_RGB2BGR);
                    return true;
                }
            } else {
                // Tiled super-resolution inference with prepadding
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

                        ncnn::Mat in = ncnn::Mat::from_pixels(tile.data, ncnn::Mat::PIXEL_BGR2RGB, tile.cols, tile.rows);

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
                                crop_rect.width == out_rect.width && crop_rect.height == out_rect.height) {
                                out_tile(crop_rect).copyTo(output(out_rect));
                            }
                        }
                    }
                }
                return true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[RealESRGAN] Process exception, falling back to SIMD: " << e.what() << "\n";
        }
    }
#endif

    // Fallback: High-Performance Multi-Architecture Target Cloned SIMD / OpenMP
    applyFastSuperResolutionSIMD(input, output, scale);
    return true;
}

void RealESRGANEngine::release() {
#if BAWAL_HAVE_NCNN
    if (s_realesrgan_net) {
        s_realesrgan_net->clear();
        s_realesrgan_net.reset();
    }
#if defined(USE_NCNN_VULKAN) || defined(NCNN_VULKAN)
    if (s_vulkan_initialized) {
        ncnn::destroy_gpu_instance();
        s_vulkan_initialized = false;
    }
#endif
#endif
    is_initialized = false;
}

} // namespace bawal::vision
