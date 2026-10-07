#include "bawal/vision/classifier.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <omp.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

namespace bawal::vision {

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
      m_env(ORT_LOGGING_LEVEL_WARNING, "BawalVisionYOLO"),
      m_input_shape{1, 3, 640, 640} {
}

bool FoodClassifier::init(std::string_view model_path, int thread_count) {
    std::lock_guard<std::mutex> lock(m_infer_mutex);
    m_is_ready = false;

    std::vector<std::string> candidate_paths;
    if (!model_path.empty()) {
        candidate_paths.emplace_back(model_path);
    }
    candidate_paths.push_back("/app/ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("/app/weights/yolo8n.onnx");
    candidate_paths.push_back("ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("../ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("../../ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("weights/yolo8n.onnx");
    candidate_paths.push_back("../weights/yolo8n.onnx");

    std::string resolved_path;
    for (const auto& path : candidate_paths) {
        std::ifstream f(path.c_str());
        if (f.good()) {
            resolved_path = path;
            break;
        }
    }

    if (resolved_path.empty()) {
        std::cerr << "[FoodClassifier] Error: File model ONNX tidak ditemukan di search path.\n";
        return false;
    }

    try {
        m_session_options.SetIntraOpNumThreads(thread_count > 0 ? thread_count : 4);
        m_session_options.SetInterOpNumThreads(1);
        m_session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

        m_session = std::make_unique<Ort::Session>(m_env, resolved_path.c_str(), m_session_options);

        Ort::AllocatorWithDefaultOptions allocator;

        // Ambil nama node input
        m_input_node_names.clear();
        m_input_names.clear();
        const size_t num_inputs = m_session->GetInputCount();
        for (size_t i = 0; i < num_inputs; ++i) {
            auto input_name_ptr = m_session->GetInputNameAllocated(i, allocator);
            m_input_node_names.push_back(input_name_ptr.get());
        }
        for (const auto& name : m_input_node_names) {
            m_input_names.push_back(name.c_str());
        }

        // Ambil nama node output
        m_output_node_names.clear();
        m_output_names.clear();
        const size_t num_outputs = m_session->GetOutputCount();
        for (size_t i = 0; i < num_outputs; ++i) {
            auto output_name_ptr = m_session->GetOutputNameAllocated(i, allocator);
            m_output_node_names.push_back(output_name_ptr.get());
        }
        for (const auto& name : m_output_node_names) {
            m_output_names.push_back(name.c_str());
        }

        m_is_ready = true;
        std::cout << "[FoodClassifier] ONNX Runtime v1.19.2 berhasil memuat model: " << resolved_path << "\n";
        return true;
    } catch (const Ort::Exception& e) {
        std::cerr << "[FoodClassifier] ONNX Runtime Init Error (" << resolved_path << "): " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[FoodClassifier] Exception: " << e.what() << "\n";
    }

    return false;
}

bool FoodClassifier::detect(const cv::Mat& frame, 
                            std::vector<DetectionResult>& out_detections,
                            float conf_threshold,
                            float nms_threshold) {
    if (frame.empty()) {
        return false;
    }

    out_detections.clear();

    std::lock_guard<std::mutex> lock(m_infer_mutex);
    if (!m_is_ready || !m_session) {
        return false;
    }

    try {
        const int target_w = 640;
        const int target_h = 640;
        const int spatial_size = target_w * target_h;

        // 1. Resize Frame
        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);

        // 2. Preprocessing Paralel (HWC BGR -> CHW RGB, Normalisasi Float)
        std::vector<float> input_tensor_values(1 * 3 * spatial_size);

        #pragma omp parallel for schedule(static)
        for (int c = 0; c < 3; ++c) {
            // Swap BGR ke RGB (c=0 -> R, c=1 -> G, c=2 -> B)
            const int src_ch = 2 - c;
            float* ch_dest = input_tensor_values.data() + (c * spatial_size);

            for (int h = 0; h < target_h; ++h) {
                const auto* row_ptr = resized.ptr<cv::Vec3b>(h);
                #pragma omp simd
                for (int w = 0; w < target_w; ++w) {
                    ch_dest[h * target_w + w] = static_cast<float>(row_ptr[w][src_ch]) * (1.0f / 255.0f);
                }
            }
        }

        // 3. Eksekusi Inferensi ONNX Runtime
        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem_info, 
            input_tensor_values.data(), 
            input_tensor_values.size(), 
            m_input_shape.data(), 
            m_input_shape.size()
        );

        auto output_tensors = m_session->Run(
            Ort::RunOptions{nullptr}, 
            m_input_names.data(), 
            &input_tensor, 
            1, 
            m_output_names.data(), 
            1
        );

        if (output_tensors.empty()) {
            return false;
        }

        // 4. Parser Tensor Output YOLOv8
        float* raw_output = output_tensors[0].GetTensorMutableData<float>();
        auto shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

        int channels = 0;
        int num_anchors = 0;
        bool is_transposed = false;

        if (shape.size() == 3) {
            if (shape[1] < shape[2]) {
                // Standar YOLOv8: [1, 13, 8400]
                channels = static_cast<int>(shape[1]);
                num_anchors = static_cast<int>(shape[2]);
                is_transposed = false;
            } else {
                // Format tertranspose: [1, 8400, 13]
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

        const float scale_x = static_cast<float>(frame.cols) / static_cast<float>(target_w);
        const float scale_y = static_cast<float>(frame.rows) / static_cast<float>(target_h);

        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;
        std::vector<int> class_ids;

        boxes.reserve(64);
        confidences.reserve(64);
        class_ids.reserve(64);

        for (int a = 0; a < num_anchors; ++a) {
            float cx, cy, w, h;
            int best_class_id = 0;
            float max_score = -1.0f;

            if (!is_transposed) {
                // Layout [1, channels, 8400]
                cx = raw_output[0 * num_anchors + a];
                cy = raw_output[1 * num_anchors + a];
                w  = raw_output[2 * num_anchors + a];
                h  = raw_output[3 * num_anchors + a];

                for (int c = 0; c < num_classes; ++c) {
                    float score = raw_output[(4 + c) * num_anchors + a];
                    if (score > max_score) {
                        max_score = score;
                        best_class_id = c;
                    }
                }
            } else {
                // Layout [1, 8400, channels]
                const float* anchor_ptr = raw_output + (a * channels);
                cx = anchor_ptr[0];
                cy = anchor_ptr[1];
                w  = anchor_ptr[2];
                h  = anchor_ptr[3];

                for (int c = 0; c < num_classes; ++c) {
                    float score = anchor_ptr[4 + c];
                    if (score > max_score) {
                        max_score = score;
                        best_class_id = c;
                    }
                }
            }

            if (max_score >= conf_threshold) {
                int left   = static_cast<int>((cx - 0.5f * w) * scale_x);
                int top    = static_cast<int>((cy - 0.5f * h) * scale_y);
                int width  = static_cast<int>(w * scale_x);
                int height = static_cast<int>(h * scale_y);

                cv::Rect box(left, top, width, height);
                box &= cv::Rect(0, 0, frame.cols, frame.rows);

                if (box.width > 4 && box.height > 4) {
                    boxes.push_back(box);
                    confidences.push_back(max_score);
                    class_ids.push_back(best_class_id);
                }
            }
        }

        // 5. Non-Maximum Suppression (NMS)
        std::vector<int> nms_indices;
        cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, nms_indices);

        out_detections.reserve(nms_indices.size());

        // 6. Evaluasi degradasi warna SIMD AVX2 pada tiap ROI
        for (int idx : nms_indices) {
            DetectionResult det;
            det.class_id = class_ids[idx];
            det.class_name = (det.class_id < static_cast<int>(m_class_names.size()))
                                 ? m_class_names[det.class_id]
                                 : ("food_class_" + std::to_string(det.class_id));
            det.confidence = confidences[idx];
            det.bbox = boxes[idx];

            if (det.bbox.width >= 8 && det.bbox.height >= 8) {
                cv::Mat roi = frame(det.bbox);
                det.spoilage = evaluateSpoilageAVX2(roi, det.class_name);
            }

            out_detections.push_back(std::move(det));
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

bool FoodClassifier::predict(const cv::Mat& bgr_image, ClassificationResult& out_result) {
    if (bgr_image.empty()) {
        return false;
    }

    std::vector<DetectionResult> detections;
    if (detect(bgr_image, detections, 0.15f, 0.45f) && !detections.empty()) {
        auto best = std::max_element(detections.begin(), detections.end(),
                                     [](const DetectionResult& a, const DetectionResult& b) {
                                         return a.confidence < b.confidence;
                                     });
        out_result = *best;
        return true;
    }

    out_result.class_id = -1;
    out_result.class_name = "no_food";
    out_result.confidence = 0.0f;
    out_result.bbox = cv::Rect(0, 0, bgr_image.cols, bgr_image.rows);
    return true;
}

} // namespace bawal::vision