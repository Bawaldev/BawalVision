#include "bawal/vision/classifier.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <utility>
#include <omp.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace bawal::vision {

void computeBGRSumsAVX2(const cv::Mat& bgr_mat, double& out_sum_b, double& out_sum_g, double& out_sum_r, int& out_sampled_pixels) {
    const int width = bgr_mat.cols;
    const int height = bgr_mat.rows;
    if (bgr_mat.empty() || width <= 0 || height <= 0) {
        out_sum_b = out_sum_g = out_sum_r = 0.0;
        out_sampled_pixels = 0;
        return;
    }

#if defined(__AVX2__)
    const __m128i m0_b = _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i m1_b = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14, -1, -1, -1, -1, -1);
    const __m128i m2_b = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 4, 7, 10, 13);

    const __m128i m0_g = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i m1_g = _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1);
    const __m128i m2_g = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14);

    const __m128i m0_r = _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i m1_r = _mm_setr_epi8(-1, -1, -1, -1, -1, 1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1);
    const __m128i m2_r = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15);

    const __m256i zero256 = _mm256_setzero_si256();
    __m256i acc_b = _mm256_setzero_si256();
    __m256i acc_g = _mm256_setzero_si256();
    __m256i acc_r = _mm256_setzero_si256();

    int sampled_pixels = 0;
    if (width >= 32) {
        for (int y = 0; y < height; y += 16) {
            const uint8_t* row_ptr = bgr_mat.ptr<uint8_t>(y);
            for (int x = 0; x <= width - 32; x += 32) {
                const uint8_t* ptr0 = row_ptr + x * 3;
                __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr0));
                __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr0 + 16));
                __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr0 + 32));

                __m128i b_low = _mm_or_si128(_mm_shuffle_epi8(v0, m0_b), _mm_or_si128(_mm_shuffle_epi8(v1, m1_b), _mm_shuffle_epi8(v2, m2_b)));
                __m128i g_low = _mm_or_si128(_mm_shuffle_epi8(v0, m0_g), _mm_or_si128(_mm_shuffle_epi8(v1, m1_g), _mm_shuffle_epi8(v2, m2_g)));
                __m128i r_low = _mm_or_si128(_mm_shuffle_epi8(v0, m0_r), _mm_or_si128(_mm_shuffle_epi8(v1, m1_r), _mm_shuffle_epi8(v2, m2_r)));

                const uint8_t* ptr1 = ptr0 + 48;
                __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr1));
                __m128i v4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr1 + 16));
                __m128i v5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr1 + 32));

                __m128i b_high = _mm_or_si128(_mm_shuffle_epi8(v3, m0_b), _mm_or_si128(_mm_shuffle_epi8(v4, m1_b), _mm_shuffle_epi8(v5, m2_b)));
                __m128i g_high = _mm_or_si128(_mm_shuffle_epi8(v3, m0_g), _mm_or_si128(_mm_shuffle_epi8(v4, m1_g), _mm_shuffle_epi8(v5, m2_g)));
                __m128i r_high = _mm_or_si128(_mm_shuffle_epi8(v3, m0_r), _mm_or_si128(_mm_shuffle_epi8(v4, m1_r), _mm_shuffle_epi8(v5, m2_r)));

                __m256i b_256 = _mm256_set_m128i(b_high, b_low);
                __m256i g_256 = _mm256_set_m128i(g_high, g_low);
                __m256i r_256 = _mm256_set_m128i(r_high, r_low);

                acc_b = _mm256_add_epi64(acc_b, _mm256_sad_epu8(b_256, zero256));
                acc_g = _mm256_add_epi64(acc_g, _mm256_sad_epu8(g_256, zero256));
                acc_r = _mm256_add_epi64(acc_r, _mm256_sad_epu8(r_256, zero256));
                sampled_pixels += 32;
            }
        }
    }

    if (sampled_pixels > 0) {
        alignas(32) uint64_t b_arr[4], g_arr[4], r_arr[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(b_arr), acc_b);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(g_arr), acc_g);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(r_arr), acc_r);

        out_sum_b = static_cast<double>(b_arr[0] + b_arr[1] + b_arr[2] + b_arr[3]);
        out_sum_g = static_cast<double>(g_arr[0] + g_arr[1] + g_arr[2] + g_arr[3]);
        out_sum_r = static_cast<double>(r_arr[0] + r_arr[1] + r_arr[2] + r_arr[3]);
        out_sampled_pixels = sampled_pixels;
        return;
    }
#endif

    // Universal / fallback sampling loop (for non-AVX2 CPUs or small ROIs)
    double sb = 0.0, sg = 0.0, sr = 0.0;
    int sampled = 0;
    const int step_y = std::max(1, height / 24);
    const int step_x = std::max(1, width / 24);

    for (int y = 0; y < height; y += step_y) {
        const uint8_t* ptr = bgr_mat.ptr<uint8_t>(y);
        for (int x = 0; x < width; x += step_x) {
            sb += ptr[x * 3];
            sg += ptr[x * 3 + 1];
            sr += ptr[x * 3 + 2];
            sampled++;
        }
    }

    out_sum_b = sb;
    out_sum_g = sg;
    out_sum_r = sr;
    out_sampled_pixels = sampled;
}

SceneOpticalMetrics evaluateSceneOptical(const cv::Mat& bgr_mat) {
    SceneOpticalMetrics metrics;
    if (bgr_mat.empty()) return metrics;

    computeBGRSumsAVX2(bgr_mat, metrics.sum_b, metrics.sum_g, metrics.sum_r, metrics.sampled_pixels);
    if (metrics.sampled_pixels <= 0) return metrics;

    metrics.avg_b = static_cast<float>(metrics.sum_b / metrics.sampled_pixels);
    metrics.avg_g = static_cast<float>(metrics.sum_g / metrics.sampled_pixels);
    metrics.avg_r = static_cast<float>(metrics.sum_r / metrics.sampled_pixels);

    // Standard ITU-R BT.601 perceptual luminance
    metrics.lux_luminance = 0.299f * metrics.avg_r + 0.587f * metrics.avg_g + 0.114f * metrics.avg_b;

    // Chromaticity balance evaluation & Correlated Color Temperature (CCT)
    const float sum_rgb = metrics.avg_r + metrics.avg_g + metrics.avg_b + 1e-4f;
    const float r_norm = metrics.avg_r / sum_rgb;
    const float b_norm = metrics.avg_b / sum_rgb;

    if (metrics.lux_luminance < 28.0f) {
        metrics.color_temperature = 4500;
        metrics.white_balance_status = "UNDEREXPOSED";
    } else if (metrics.lux_luminance > 235.0f) {
        metrics.color_temperature = 6500;
        metrics.white_balance_status = "OVEREXPOSED";
    } else if (r_norm > 0.44f && b_norm < 0.26f) {
        metrics.color_temperature = 3200; // Warm incandescent / halogen
        metrics.white_balance_status = "WARM";
    } else if (b_norm > 0.40f && r_norm < 0.26f) {
        metrics.color_temperature = 8200; // Cool / blue cast
        metrics.white_balance_status = "COOL";
    } else {
        metrics.color_temperature = 6000; // Balanced neutral kitchen daylight
        metrics.white_balance_status = "BALANCED";
    }

    return metrics;
}

namespace {

inline float computeIoU(const CompactBBox& a, const CompactBBox& b) {
    int inter_x1 = std::max(a.x, b.x);
    int inter_y1 = std::max(a.y, b.y);
    int inter_x2 = std::min(a.x + a.width, b.x + b.width);
    int inter_y2 = std::min(a.y + a.height, b.y + b.height);

    int inter_w = std::max(0, inter_x2 - inter_x1);
    int inter_h = std::max(0, inter_y2 - inter_y1);
    float inter_area = static_cast<float>(inter_w * inter_h);

    float union_area = static_cast<float>(a.width * a.height + b.width * b.height) - inter_area;
    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

} // anonymous namespace

FoodClassifier::FoodClassifier() 
    : m_class_names{
        "banana",
        "chicken",
        "egg",
        "orange",
        "rice",
        "spinach",
        "tempeh",
        "tofu",
        "water_spinach"
    },
      m_env(ORT_LOGGING_LEVEL_WARNING, "BawalVisionYOLO") {
}

bool FoodClassifier::init(std::string_view model_path, int thread_count) {
    std::lock_guard<std::mutex> lock(m_infer_mutex);
    m_is_ready.store(false, std::memory_order_release);

    // Pre-allocate every reusable buffer exactly once. All later calls only
    // clear/resize within existing capacity, so the 30 FPS loop never touches
    // the allocator for post-processing scratch data.
    m_resized_buffer.create(640, 640, CV_8UC3);
    m_scratch_boxes.reserve(128);
    m_scratch_confidences.reserve(128);
    m_scratch_class_ids.reserve(128);
    m_scratch_nms_indices.reserve(128);
    m_scratch_detections.reserve(64);
    m_scratch_matched.reserve(64);
    m_tracked_items.reserve(32);

    std::vector<std::string> candidate_paths;
    if (!model_path.empty()) {
        candidate_paths.emplace_back(model_path);
    }

    // PRIORITY 1: YOLOv8s INT8 (Quantized for AVX2 VPMADDUBSW / oneDNN)
    candidate_paths.push_back("/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("/app/weights/yolov8s_int8.onnx");
    candidate_paths.push_back("/app/weights/yolo8s_int8.onnx");
    candidate_paths.push_back("weights/yolov8s_int8.onnx");
    candidate_paths.push_back("weights/yolo8s_int8.onnx");
    candidate_paths.push_back("../weights/yolov8s_int8.onnx");
    candidate_paths.push_back("../../weights/yolov8s_int8.onnx");

    // PRIORITY 2: YOLOv8s FP32 (Full Precision Small Model)
    candidate_paths.push_back("/app/ml/AI1/model/yolov8s/ai1_vision.onnx");
    candidate_paths.push_back("/app/ml/AI1/model/yolov8s/yolov8s.onnx");
    candidate_paths.push_back("ml/AI1/model/yolov8s/ai1_vision.onnx");
    candidate_paths.push_back("ml/AI1/model/yolov8s/yolov8s.onnx");
    candidate_paths.push_back("../ml/AI1/model/yolov8s/ai1_vision.onnx");
    candidate_paths.push_back("../../ml/AI1/model/yolov8s/ai1_vision.onnx");
    candidate_paths.push_back("../../../ml/AI1/model/yolov8s/ai1_vision.onnx");
    candidate_paths.push_back("/app/weights/yolov8s.onnx");
    candidate_paths.push_back("weights/yolov8s.onnx");
    candidate_paths.push_back("../weights/yolov8s.onnx");
    candidate_paths.push_back("../../weights/yolov8s.onnx");

    // PRIORITY 3: Fallback to YOLOv8n INT8 / Generic ai1_vision_int8.onnx
    candidate_paths.push_back("/app/ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx");
    candidate_paths.push_back("/app/weights/ai1_vision_int8.onnx");
    candidate_paths.push_back("weights/ai1_vision_int8.onnx");
    candidate_paths.push_back("../weights/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../weights/ai1_vision_int8.onnx");
    candidate_paths.push_back("../../../weights/ai1_vision_int8.onnx");

    // PRIORITY 4: Fallback to YOLOv8n FP32
    candidate_paths.push_back("/app/ml/AI1/model/yolov8n/ai1_vision.onnx");
    candidate_paths.push_back("ml/AI1/model/yolov8n/ai1_vision.onnx");
    candidate_paths.push_back("../ml/AI1/model/yolov8n/ai1_vision.onnx");
    candidate_paths.push_back("../../ml/AI1/model/yolov8n/ai1_vision.onnx");

    for (const auto& path : candidate_paths) {
        std::ifstream f(path.c_str());
        if (!f.good()) {
            continue;
        }

        try {
            m_session_options.SetIntraOpNumThreads(thread_count > 0 ? thread_count : 4);
            m_session_options.SetInterOpNumThreads(1);
            m_session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            m_session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

#if defined(USE_DNNL) || defined(HAVE_DNNL)
            try {
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_Dnnl(m_session_options, 1));
                std::cout << "[FoodClassifier] Intel oneDNN (DNNL) Execution Provider registered for Xeon acceleration.\n";
            } catch (const std::exception& e) {
                // Default to standard high-performance CPU execution provider
            }
#endif

            m_session = std::make_unique<Ort::Session>(m_env, path.c_str(), m_session_options);

            // Preprocessing buffer with 64-byte alignment for AVX2/AVX-512 vectorization
            m_input_tensor_values.reset(
                static_cast<float*>(::operator new[](K_TENSOR_ELEMENTS * sizeof(float), std::align_val_t{64}))
            );
            m_tracked_items.clear();
            m_reset_tracker.store(false, std::memory_order_relaxed);
            m_is_ready.store(true, std::memory_order_release);
            m_active_model_path = path;

            // Detect architecture from model filename
            std::string lower_path = path;
            for (char& c : lower_path) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool is_yolo8s = (lower_path.find("yolov8s") != std::string::npos || lower_path.find("yolo8s") != std::string::npos);
            const bool is_int8 = (lower_path.find("int8") != std::string::npos);

            if (is_yolo8s && is_int8) {
                m_model_arch = "YOLOv8s-INT8";
            } else if (is_yolo8s) {
                m_model_arch = "YOLOv8s";
            } else if (is_int8) {
                m_model_arch = "YOLOv8n-INT8";
            } else {
                m_model_arch = "YOLOv8n";
            }

            std::cout << "[FoodClassifier] ONNX Runtime loaded model: " << path 
                      << " [Architecture: " << m_model_arch << "]\n";
            return true;
        } catch (const Ort::Exception& e) {
            std::cerr << "[FoodClassifier] Error loading model (" << path << "): " << e.what() << "\n";
            m_session.reset();
        } catch (const std::exception& e) {
            std::cerr << "[FoodClassifier] Exception loading model (" << path << "): " << e.what() << "\n";
            m_session.reset();
        }
    }

    std::cerr << "[FoodClassifier] Error: No suitable ONNX model (YOLOv8s or YOLOv8n) could be initialized.\n";
    return false;
}

bool FoodClassifier::detect(const cv::Mat& frame, 
                            std::span<DetectionResult> out_buffer,
                            size_t& out_count,
                            float conf_threshold,
                            float nms_threshold) {
    out_count = 0;
    if (frame.empty() || out_buffer.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_infer_mutex);
    if (!m_is_ready.load(std::memory_order_acquire) || !m_session) {
        return false;
    }

    // Consume a pending non-blocking tracker reset before doing any work.
    resetTrackerLocked();

    try {
        // High-speed vectorized AVX2 scene optical analysis & illuminance telemetry.
        // Publish the snapshot under a short-lived dedicated mutex so the stream
        // thread never races with the AI worker or waits for the full inference.
        {
            SceneOpticalMetrics optical = evaluateSceneOptical(frame);
            std::lock_guard<std::mutex> optical_lock(m_optical_mutex);
            m_last_optical = std::move(optical);
        }

        constexpr int target_w = 640;
        constexpr int target_h = 640;
        constexpr int spatial_size = target_w * target_h;

        // 1. Aspect-Ratio Preserving Letterbox Preprocessing (640x640 with 114 border pad)
        const float scale = std::min(static_cast<float>(target_w) / frame.cols,
                                     static_cast<float>(target_h) / frame.rows);
        const int new_w = std::min(target_w, std::max(1, static_cast<int>(std::round(frame.cols * scale))));
        const int new_h = std::min(target_h, std::max(1, static_cast<int>(std::round(frame.rows * scale))));
        const int pad_x = (target_w - new_w) / 2;
        const int pad_y = (target_h - new_h) / 2;

        cv::Mat scaled_frame;
        cv::resize(frame, scaled_frame, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

        // Optimal zero-redundancy border padding (only writes pad borders, avoiding 1.2MB full setTo clear)
        const int pad_bottom = target_h - new_h - pad_y;
        const int pad_right  = target_w - new_w - pad_x;
        cv::copyMakeBorder(scaled_frame, m_resized_buffer, pad_y, pad_bottom, pad_x, pad_right,
                           cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));

        // 2. Parallel Preprocessing into Persistent Buffer (HWC BGR -> CHW RGB, Standard [0.0, 1.0] Normalization)
        // Passes pure, unshifted RGB tensor to neural network matching Ultralytics training pipeline
        float* const __restrict tensor_ptr = m_input_tensor_values.get();
        if (!tensor_ptr) {
            return false;
        }
        constexpr float inv255 = 1.0f / 255.0f;

        // Pure per-row pixel transformation: no shared container writes.
        // schedule(runtime) lets the runtime adapt to hybrid/homogeneous CPUs.
<<<<<<< HEAD
        #pragma omp parallel for schedule(runtime)
=======
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
        for (int h = 0; h < target_h; ++h) {
            const auto* __restrict row_ptr = m_resized_buffer.ptr<cv::Vec3b>(h);
            const int row_offset = h * target_w;
            #pragma omp simd
            for (int w = 0; w < target_w; ++w) {
                const auto& pixel = row_ptr[w];
                tensor_ptr[0 * spatial_size + row_offset + w] = static_cast<float>(pixel[2]) * inv255;
                tensor_ptr[1 * spatial_size + row_offset + w] = static_cast<float>(pixel[1]) * inv255;
                tensor_ptr[2 * spatial_size + row_offset + w] = static_cast<float>(pixel[0]) * inv255;
            }
        }

        // 3. Execute ONNX Runtime Inference with static node names and 64-byte aligned tensor
        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem_info, 
            tensor_ptr, 
            K_TENSOR_ELEMENTS, 
            K_INPUT_SHAPE.data(), 
            K_INPUT_SHAPE.size()
        );

        auto output_tensors = m_session->Run(
            Ort::RunOptions{nullptr}, 
            K_INPUT_NAMES, 
            &input_tensor, 
            1, 
            K_OUTPUT_NAMES, 
            1
        );

        if (output_tensors.empty()) {
            return false;
        }

        auto type_info = output_tensors[0].GetTensorTypeAndShapeInfo();
        auto element_type = type_info.GetElementType();

        // Runtime validation of output tensor data type
        if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            std::cerr << "[FoodClassifier] Error: Output tensor has element type " 
                    << element_type << " (expected FLOAT32). "
                    << "Ensure INT8 quantization preserves output node as dequantized float!\n";
            return false;
        }

        float* raw_output = output_tensors[0].GetTensorMutableData<float>();
        auto shape = type_info.GetShape();

        int channels = 0;
        int num_anchors = 0;
        bool is_transposed = false;

        if (shape.size() == 3) {
            if (shape[1] < shape[2]) {
                channels = static_cast<int>(shape[1]);
                num_anchors = static_cast<int>(shape[2]);
                is_transposed = false;
            } else {
                num_anchors = static_cast<int>(shape[1]);
                channels = static_cast<int>(shape[2]);
                is_transposed = true;
            }
        } else {
            return false;
        }

        const int num_classes = channels - 4;
        if (num_classes <= 0) {
            return false;
        }

        // Reuse pre-allocated scratch buffers (capacity reserved in init()).
        std::vector<cv::Rect>& boxes = m_scratch_boxes;
        std::vector<float>& confidences = m_scratch_confidences;
        std::vector<int>& class_ids = m_scratch_class_ids;
        boxes.clear();
        confidences.clear();
        class_ids.clear();

        // Vectorized candidate box parsing with early score threshold rejection (bypassing coordinate decode for >95% anchors)
        for (int a = 0; a < num_anchors; ++a) {
            int best_class_id = 0;
            float max_score = -1.0f;

            if (!is_transposed) {
                // Find maximum class score first across channels
                max_score = raw_output[4 * num_anchors + a];
                best_class_id = 0;
                for (int c = 1; c < num_classes; ++c) {
                    const float score = raw_output[(4 + c) * num_anchors + a];
                    if (score > max_score) {
                        max_score = score;
                        best_class_id = c;
                    }
                }

                // Early rejection: avoid reading cx, cy, w, h and performing geometric calculations
                if (max_score < conf_threshold) {
                    continue;
                }

                const float cx = raw_output[0 * num_anchors + a];
                const float cy = raw_output[1 * num_anchors + a];
                const float w  = raw_output[2 * num_anchors + a];
                const float h  = raw_output[3 * num_anchors + a];

                const float unpad_cx = cx - static_cast<float>(pad_x);
                const float unpad_cy = cy - static_cast<float>(pad_y);
                const float orig_left = (unpad_cx - 0.5f * w) / scale;
                const float orig_top  = (unpad_cy - 0.5f * h) / scale;
                const float orig_w    = w / scale;
                const float orig_h    = h / scale;

                const int left   = static_cast<int>(std::round(orig_left));
                const int top    = static_cast<int>(std::round(orig_top));
                const int width  = static_cast<int>(std::round(orig_w));
                const int height = static_cast<int>(std::round(orig_h));

                cv::Rect box(left, top, width, height);
                box &= cv::Rect(0, 0, frame.cols, frame.rows);

                if (box.width > 16 && box.height > 16) {
                    boxes.push_back(box);
                    confidences.push_back(max_score);
                    class_ids.push_back(best_class_id);
                }
            } else {
                const float* anchor_ptr = raw_output + (a * channels);
                max_score = anchor_ptr[4];
                best_class_id = 0;
                for (int c = 1; c < num_classes; ++c) {
                    const float score = anchor_ptr[4 + c];
                    if (score > max_score) {
                        max_score = score;
                        best_class_id = c;
                    }
                }

                if (max_score < conf_threshold) {
                    continue;
                }

                const float cx = anchor_ptr[0];
                const float cy = anchor_ptr[1];
                const float w  = anchor_ptr[2];
                const float h  = anchor_ptr[3];

                const float unpad_cx = cx - static_cast<float>(pad_x);
                const float unpad_cy = cy - static_cast<float>(pad_y);
                const float orig_left = (unpad_cx - 0.5f * w) / scale;
                const float orig_top  = (unpad_cy - 0.5f * h) / scale;
                const float orig_w    = w / scale;
                const float orig_h    = h / scale;

                const int left   = static_cast<int>(std::round(orig_left));
                const int top    = static_cast<int>(std::round(orig_top));
                const int width  = static_cast<int>(std::round(orig_w));
                const int height = static_cast<int>(std::round(orig_h));

                cv::Rect box(left, top, width, height);
                box &= cv::Rect(0, 0, frame.cols, frame.rows);

                if (box.width > 16 && box.height > 16) {
                    boxes.push_back(box);
                    confidences.push_back(max_score);
                    class_ids.push_back(best_class_id);
                }
            }
        }

        // 5. Non-Maximum Suppression (NMS)
        std::vector<int>& nms_indices = m_scratch_nms_indices;
        nms_indices.clear();
        cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, nms_indices);

        // Age existing tracks for one frame and emit the still-visible ones.
        // Used both when YOLO has no candidates and when all candidates are
        // filtered out (flat texture/size), so a single bad frame does not
        // wipe a stable track.
        auto age_tracks_and_emit = [&]() {
            for (auto& item : m_tracked_items) {
                item.lost_frames++;
                item.result.confidence *= 0.90f;
            }
            m_tracked_items.erase(
                std::remove_if(m_tracked_items.begin(), m_tracked_items.end(),
                               [](const TrackedItem& item) { return item.lost_frames > 2; }),
                m_tracked_items.end());

            out_count = 0;
            const size_t max_capacity = out_buffer.size();
            for (const auto& item : m_tracked_items) {
                if (item.lost_frames <= 1) {
                    if (out_count < max_capacity) {
                        out_buffer[out_count++] = item.result;
                    } else {
                        break;
                    }
                }
            }
            return true;
        };

        // If YOLO detects no candidates this frame, age existing tracks instead of clearing immediately
        if (nms_indices.empty()) {
<<<<<<< HEAD
            for (auto& item : m_tracked_items) {
                item.lost_frames++;
                item.result.confidence *= 0.90f;
            }
            m_tracked_items.erase(
                std::remove_if(m_tracked_items.begin(), m_tracked_items.end(),
                               [](const TrackedItem& item) { return item.lost_frames > 2; }),
                m_tracked_items.end());

            out_count = 0;
            const size_t max_capacity = out_buffer.size();
            for (const auto& item : m_tracked_items) {
                if (item.lost_frames <= 1) {
                    if (out_count < max_capacity) {
                        out_buffer[out_count++] = item.result;
                    } else {
                        break;
                    }
                }
            }
            return true;
=======
            return age_tracks_and_emit();
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
        }

        std::vector<DetectionResult>& current_detections = m_scratch_detections;
        current_detections.clear();

        // 6. Build High-Precision Detections from YOLO Neural Inference
        for (int idx : nms_indices) {
            cv::Rect final_box = boxes[idx];
            final_box &= cv::Rect(0, 0, frame.cols, frame.rows);

            if (final_box.width < 16 || final_box.height < 16) {
                continue;
            }

            int yolo_cls = class_ids[idx];
            float yolo_score = confidences[idx];

            std::string_view predicted_name = (yolo_cls >= 0 && yolo_cls < static_cast<int>(m_class_names.size()))
                                                 ? m_class_names[yolo_cls]
                                                 : std::string_view("food_item");

            DetectionResult det;
            det.bbox = final_box;
            det.class_id = yolo_cls;
            det.class_name = predicted_name;
            det.confidence = std::clamp(yolo_score, 0.0f, 1.0f);

            // AVX2 SIMD texture validity filter (only reject flat walls/tables if YOLO confidence is uncertain < 0.75)
            cv::Mat final_food_roi = frame(det.bbox);
            if (det.confidence < 0.75f) {
                TextureMetric texture = evaluateTextureAVX2(final_food_roi);
                if (!texture.is_valid_food_texture) {
                    continue;
                }
            }

            // AVX2 SIMD color spoilage and freshness assessment
            det.spoilage = evaluateSpoilageAVX2(final_food_roi, det.class_name);

            current_detections.push_back(std::move(det));
        }

        if (current_detections.empty()) {
<<<<<<< HEAD
            m_tracked_items.clear();
            out_count = 0;
            return true;
=======
            return age_tracks_and_emit();
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
        }

        // 8. Temporal Tracking & Smoothing (Stabilizes Bounding Boxes & Suppresses Flicker)
        std::vector<uint8_t>& matched = m_scratch_matched;
        matched.resize(current_detections.size(), 0);
        std::fill(matched.begin(), matched.end(), uint8_t{0});

        for (auto& tracked : m_tracked_items) {
            float best_iou = 0.0f;
            int best_idx = -1;

            for (size_t i = 0; i < current_detections.size(); ++i) {
                if (matched[i]) continue;
                float iou = computeIoU(tracked.result.bbox, current_detections[i].bbox);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_idx = static_cast<int>(i);
                }
            }

            if (best_iou > 0.15f && best_idx >= 0) {
                matched[best_idx] = true;
                const auto& cur = current_detections[best_idx];

                // Compute bounding box center displacement and size delta
                const cv::Point cur_center(cur.bbox.x + cur.bbox.width / 2, cur.bbox.y + cur.bbox.height / 2);
                const int displacement = std::abs(cur_center.x - tracked.last_center.x) + std::abs(cur_center.y - tracked.last_center.y);
                const int size_diff = std::abs(cur.bbox.width - tracked.result.bbox.width) + std::abs(cur.bbox.height - tracked.result.bbox.height);
                tracked.last_center = cur_center;

                // Static motion stability detection:
                // Bounding box displacement <= 8 px and size change <= 10 px counts as stationary
                if (displacement <= 8 && size_diff <= 10) {
                    tracked.stable_frames++;
                } else {
                    tracked.stable_frames = 0;
                }

                // EMA Bounding Box Smoothing
                tracked.result.bbox.x = static_cast<int>(0.70f * cur.bbox.x + 0.30f * tracked.result.bbox.x);
                tracked.result.bbox.y = static_cast<int>(0.70f * cur.bbox.y + 0.30f * tracked.result.bbox.y);
                tracked.result.bbox.width = static_cast<int>(0.70f * cur.bbox.width + 0.30f * tracked.result.bbox.width);
                tracked.result.bbox.height = static_cast<int>(0.70f * cur.bbox.height + 0.30f * tracked.result.bbox.height);

                tracked.result.class_name = cur.class_name;
                tracked.result.class_id = cur.class_id;
                tracked.result.confidence = 0.70f * cur.confidence + 0.30f * tracked.result.confidence;

                // Motion / Stability Filter:
                // Only update spoilage evaluation when the container/plate has remained static for >= 2 AI passes (~0.4s).
                // If the food container is in motion or being occluded by passing hands, lock the previous stable rating!
                if (tracked.stable_frames >= 2) {
                    // Smooth discoloration ratio with EMA to eliminate frame-to-frame noise
                    tracked.result.spoilage.avg_luminance = 0.30f * cur.spoilage.avg_luminance + 0.70f * tracked.result.spoilage.avg_luminance;
                    tracked.result.spoilage.discoloration_ratio = 0.30f * cur.spoilage.discoloration_ratio + 0.70f * tracked.result.spoilage.discoloration_ratio;
                    tracked.result.spoilage.is_visually_spoiled = (tracked.result.spoilage.discoloration_ratio > 0.35f);
                } else if (tracked.age <= 1) {
                    tracked.result.spoilage = cur.spoilage;
                }
                tracked.lost_frames = 0;
                tracked.age++;
            } else {
                tracked.lost_frames++;
                tracked.result.confidence *= 0.90f;
            }
        }

        // Register new untracked detections
        for (size_t i = 0; i < current_detections.size(); ++i) {
            if (!matched[i]) {
                TrackedItem new_item;
                new_item.result = current_detections[i];
                new_item.age = 1;
                new_item.lost_frames = 0;
                new_item.stable_frames = 0;
                new_item.last_center = cv::Point(new_item.result.bbox.x + new_item.result.bbox.width / 2,
                                                 new_item.result.bbox.y + new_item.result.bbox.height / 2);
                m_tracked_items.push_back(std::move(new_item));
            }
        }

        // Remove tracks lost for more than 2 consecutive frames
        m_tracked_items.erase(
            std::remove_if(m_tracked_items.begin(), m_tracked_items.end(),
                           [](const TrackedItem& item) { return item.lost_frames > 2; }),
            m_tracked_items.end());

        // Dispatch valid tracks directly into caller-provided span buffer
        out_count = 0;
        const size_t max_capacity = out_buffer.size();
        for (const auto& item : m_tracked_items) {
            if (item.lost_frames <= 1) {
                if (out_count < max_capacity) {
                    out_buffer[out_count++] = item.result;
                } else {
                    break;
                }
            }
        }

        return true;
    } catch (const Ort::Exception& e) {
        std::cerr << "[FoodClassifier] ONNX Runtime Detection Error: " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[FoodClassifier] Detect Exception: " << e.what() << "\n";
        return false;
    }
}

bool FoodClassifier::detect(const cv::Mat& frame, 
                            std::vector<DetectionResult>& out_detections,
                            float conf_threshold,
                            float nms_threshold) {
    out_detections.clear();
    std::array<DetectionResult, 32> local_buf;
    size_t count = 0;
    if (!detect(frame, local_buf, count, conf_threshold, nms_threshold)) {
        return false;
    }
    out_detections.assign(local_buf.begin(), local_buf.begin() + count);
    return true;
}

bool FoodClassifier::predict(const cv::Mat& bgr_image, ClassificationResult& out_result) {
    if (bgr_image.empty()) {
        return false;
    }

    std::array<DetectionResult, 16> local_buf;
    size_t count = 0;

    const bool ok = detect(bgr_image, local_buf, count, 0.58f, 0.45f);

    if (ok && count > 0) {
        float best_conf = -1.0f;
        size_t best_idx = 0;
        for (size_t i = 0; i < count; ++i) {
            if (local_buf[i].confidence > best_conf) {
                best_conf = local_buf[i].confidence;
                best_idx = i;
            }
        }
        out_result = local_buf[best_idx];
        return true;
    }

    out_result.class_id = -1;
    out_result.class_name = "no_food";
    out_result.confidence = 0.0f;
    out_result.bbox = cv::Rect(0, 0, bgr_image.cols, bgr_image.rows);
    return true;
}

} // namespace bawal::vision