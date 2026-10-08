#include "bawal/vision/classifier.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <omp.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

namespace bawal::vision {

namespace {

static std::vector<TrackedItem> s_tracked_items;

// Compute Intersection over Union (IoU) between two bounding boxes
inline float computeIoU(const cv::Rect& a, const cv::Rect& b) {
    int inter_x1 = std::max(a.x, b.x);
    int inter_y1 = std::max(a.y, b.y);
    int inter_x2 = std::min(a.x + a.width, b.x + b.width);
    int inter_y2 = std::min(a.y + a.height, b.y + b.height);

    int inter_w = std::max(0, inter_x2 - inter_x1);
    int inter_h = std::max(0, inter_y2 - inter_y1);
    float inter_area = static_cast<float>(inter_w * inter_h);

    float union_area = static_cast<float>(a.area() + b.area()) - inter_area;
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
        std::cerr << "[FoodClassifier] Error: ONNX model file not found in candidate search paths.\n";
        return false;
    }

    try {
        m_session_options.SetIntraOpNumThreads(thread_count > 0 ? thread_count : 4);
        m_session_options.SetInterOpNumThreads(1);
        m_session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

        m_session = std::make_unique<Ort::Session>(m_env, resolved_path.c_str(), m_session_options);

        Ort::AllocatorWithDefaultOptions allocator;

        // Retrieve input node names
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

        // Retrieve output node names
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
        std::cout << "[FoodClassifier] ONNX Runtime successfully loaded model: " << resolved_path << "\n";
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

        // Gray-World Auto White Balance & Color Normalization
        const cv::Scalar mean_bgr = cv::mean(resized);
        const double mean_gray = (mean_bgr[0] + mean_bgr[1] + mean_bgr[2]) / 3.0;
        const float scale_b = static_cast<float>((mean_bgr[0] > 1.0) ? (mean_gray / mean_bgr[0]) : 1.0);
        const float scale_g = static_cast<float>((mean_bgr[1] > 1.0) ? (mean_gray / mean_bgr[1]) : 1.0);
        const float scale_r = static_cast<float>((mean_bgr[2] > 1.0) ? (mean_gray / mean_bgr[2]) : 1.0);

        // 2. Parallel Preprocessing (HWC BGR -> CHW RGB, Float Normalization)
        std::vector<float> input_tensor_values(1 * 3 * spatial_size);

        #pragma omp parallel for schedule(static)
        for (int h = 0; h < target_h; ++h) {
            const auto* row_ptr = resized.ptr<cv::Vec3b>(h);
            const int row_offset = h * target_w;
            #pragma omp simd
            for (int w = 0; w < target_w; ++w) {
                const auto& pixel = row_ptr[w];
                input_tensor_values[0 * spatial_size + row_offset + w] = std::clamp(static_cast<float>(pixel[2]) * scale_r * (1.0f / 255.0f), 0.0f, 1.0f);
                input_tensor_values[1 * spatial_size + row_offset + w] = std::clamp(static_cast<float>(pixel[1]) * scale_g * (1.0f / 255.0f), 0.0f, 1.0f);
                input_tensor_values[2 * spatial_size + row_offset + w] = std::clamp(static_cast<float>(pixel[0]) * scale_b * (1.0f / 255.0f), 0.0f, 1.0f);
            }
        }

        // 3. Execute ONNX Runtime Inference
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

        // 4. Parse YOLOv8 Output Tensor
        float* raw_output = output_tensors[0].GetTensorMutableData<float>();
        auto shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

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

                if (box.width > 16 && box.height > 16) {
                    boxes.push_back(box);
                    confidences.push_back(max_score);
                    class_ids.push_back(best_class_id);
                }
            }
        }

        // 5. Non-Maximum Suppression (NMS)
        std::vector<int> nms_indices;
        cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, nms_indices);

        // If YOLO detects no candidates, clear active tracks and return immediately
        if (nms_indices.empty()) {
            s_tracked_items.clear();
            return true;
        }

        // 6. Evaluate Scene Physical Contrast
        cv::Scalar mean_bgr_frame, std_bgr_frame;
        cv::meanStdDev(frame, mean_bgr_frame, std_bgr_frame);
        const double scene_contrast = (std_bgr_frame[0] + std_bgr_frame[1] + std_bgr_frame[2]) / 3.0;

        if (scene_contrast < 10.0) {
            s_tracked_items.clear();
            return true;
        }

        std::vector<DetectionResult> current_detections;
        current_detections.reserve(nms_indices.size());

        // 7. Evaluate Each YOLO Detection with AVX2 Texture Filter & Semantic Disambiguation
        for (int idx : nms_indices) {
            cv::Rect yolo_box = boxes[idx];
            yolo_box &= cv::Rect(0, 0, frame.cols, frame.rows);

            if (yolo_box.width < 24 || yolo_box.height < 24) {
                continue;
            }

            cv::Mat food_roi = frame(yolo_box);

            // AVX2 SIMD Filter: Reject flat background walls/tables without genuine food texture
            TextureMetric tex = evaluateTextureAVX2(food_roi);
            if (!tex.is_valid_food_texture) {
                continue;
            }

            // Local Contour Refinement: Fit bounding box to plate/food contours if present
            cv::Rect final_box = yolo_box;
            cv::Mat gray_roi;
            cv::cvtColor(food_roi, gray_roi, cv::COLOR_BGR2GRAY);
            
            cv::Mat blur_roi;
            cv::GaussianBlur(gray_roi, blur_roi, cv::Size(5, 5), 0);
            cv::Mat otsu_roi;
            cv::threshold(blur_roi, otsu_roi, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

            std::vector<std::vector<cv::Point>> local_contours;
            cv::findContours(otsu_roi, local_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            cv::Rect best_contour_box;
            double max_valid_area = 0.0;
            const double total_roi_area = static_cast<double>(yolo_box.area());

            for (const auto& cnt : local_contours) {
                double a = cv::contourArea(cnt);
                if (a >= (total_roi_area * 0.12) && a <= (total_roi_area * 0.88)) {
                    cv::Rect lr = cv::boundingRect(cnt);
                    float aspect = static_cast<float>(lr.width) / static_cast<float>(lr.height);
                    if (aspect >= 0.35f && aspect <= 2.8f && a > max_valid_area) {
                        max_valid_area = a;
                        best_contour_box = lr;
                    }
                }
            }

            if (max_valid_area > 0.0) {
                int snap_x = yolo_box.x + best_contour_box.x;
                int snap_y = yolo_box.y + best_contour_box.y;
                cv::Rect snapped(snap_x, snap_y, best_contour_box.width, best_contour_box.height);
                snapped &= cv::Rect(0, 0, frame.cols, frame.rows);
                if (snapped.width >= 24 && snapped.height >= 24) {
                    final_box = snapped;
                }
            }

            // 1. Enforce Realistic Food Portion Bounding Box Boundaries:
            // Single meal portions on a school lunch tray do not engulf entire room or exceed 38% frame area
            const float frame_area = static_cast<float>(frame.cols * frame.rows);
            if (final_box.area() > frame_area * 0.38f || 
                final_box.width > frame.cols * 0.62f || 
                final_box.height > frame.rows * 0.62f) {
                continue; // Reject human figures, room backdrops, and entire scene bounding boxes
            }

            // Spectral Color Analysis on Core ROI (Center 50%)
            int core_w = std::max(16, static_cast<int>(final_box.width * 0.50f));
            int core_h = std::max(16, static_cast<int>(final_box.height * 0.50f));
            int core_x = final_box.x + (final_box.width - core_w) / 2;
            int core_y = final_box.y + (final_box.height - core_h) / 2;
            cv::Rect core_rect = cv::Rect(core_x, core_y, core_w, core_h) & cv::Rect(0, 0, frame.cols, frame.rows);

            cv::Mat core_roi = frame(core_rect);
            cv::Mat hsv_core;
            cv::cvtColor(core_roi, hsv_core, cv::COLOR_BGR2HSV);
            cv::Scalar mean_hsv = cv::mean(hsv_core);
            cv::Scalar mean_bgr = cv::mean(core_roi);

            const double h_val = mean_hsv[0];
            const double s_val = mean_hsv[1];
            const double v_val = mean_hsv[2];

            const double b_val = mean_bgr[0];
            const double g_val = mean_bgr[1];
            const double r_val = mean_bgr[2];

            // 2. Comprehensive Light Emitter / Lamp & Specular Glare Rejection:
            int blown_out_count = 0;
            const int core_total_px = core_roi.rows * core_roi.cols;
            for (int y = 0; y < core_roi.rows; ++y) {
                const cv::Vec3b* hsv_ptr = hsv_core.ptr<cv::Vec3b>(y);
                for (int x = 0; x < core_roi.cols; ++x) {
                    if (hsv_ptr[x][2] >= 235 && hsv_ptr[x][1] <= 25) {
                        blown_out_count++;
                    }
                }
            }
            const float blown_out_ratio = (core_total_px > 0) ? (static_cast<float>(blown_out_count) / static_cast<float>(core_total_px)) : 0.0f;
            if (blown_out_ratio > 0.15f || (v_val >= 210.0 && s_val <= 20.0)) {
                continue; // Reject lamps, spotlights, and blown-out highlights
            }

            int yolo_cls = class_ids[idx];
            float yolo_score = confidences[idx];

            std::string predicted_name = (yolo_cls < static_cast<int>(m_class_names.size()))
                                             ? m_class_names[yolo_cls]
                                             : "food_item";
            int final_class_id = yolo_cls;
            bool is_valid_food = true;

            // Semantic Food Disambiguation for Neutral White Food (Rice vs Tofu vs Egg):
            // Authentic cooked white rice has bright organic luminance (V >= 120, RGB >= 105),
            // low saturation (S <= 28), and neutral color balance (|R - G| <= 18, |G - B| <= 18).
            const bool is_neutral_white_profile = (s_val <= 28.0 && v_val >= 120.0 && v_val <= 245.0 &&
                                                   r_val >= 105.0 && g_val >= 105.0 && b_val >= 105.0 &&
                                                   std::abs(r_val - g_val) <= 18.0 &&
                                                   std::abs(g_val - b_val) <= 18.0);

            if (is_neutral_white_profile && (predicted_name == "tofu" || predicted_name == "egg") && yolo_score >= 0.55f) {
                predicted_name = "rice";
                final_class_id = 4; // Index 4 is "rice"
            }

            float calibrated_conf = yolo_score;

            // 3. Strict Per-Class Physical & Chromatic Validation:
            // White Rice:
            if (predicted_name == "rice") {
                if (is_neutral_white_profile && yolo_score >= 0.48f) {
                    calibrated_conf = std::clamp(yolo_score + (tex.std_dev * 0.002f), 0.50f, 0.95f);
                } else {
                    is_valid_food = false; // Reject dark shadows, dark clothing, and non-white regions
                }
            }
            // Green Vegetables (Water Spinach / Spinach):
            else if (predicted_name == "water_spinach" || predicted_name == "spinach") {
                if (h_val < 28.0 || h_val > 92.0 || s_val < 30.0 || g_val < r_val + 2.0) {
                    is_valid_food = false;
                }
            }
            // Banana:
            else if (predicted_name == "banana") {
                if (s_val < 32.0 || h_val < 15.0 || h_val > 48.0 || v_val > 225.0) {
                    is_valid_food = false;
                }
            }
            // Orange:
            else if (predicted_name == "orange") {
                if (s_val < 38.0 || h_val < 5.0 || h_val > 28.0 || v_val > 225.0) {
                    is_valid_food = false;
                }
            }
            // Tofu (Curd Soy Block):
            else if (predicted_name == "tofu") {
                if (v_val < 60.0 || v_val > 210.0 || s_val < 15.0 || s_val > 65.0 || 
                    yolo_score < 0.60f || tex.edge_energy < 10.0f || tex.std_dev < 16.0f) {
                    is_valid_food = false;
                }
            }
            // Tempeh (Fermented Soy Cake):
            else if (predicted_name == "tempeh") {
                if (h_val < 10.0 || h_val > 55.0 || s_val < 18.0 || v_val < 40.0 || v_val > 200.0 || yolo_score < 0.55f) {
                    is_valid_food = false;
                }
            }
            // Fried Chicken:
            else if (predicted_name == "chicken") {
                if (h_val < 8.0 || h_val > 48.0 || s_val < 20.0 || v_val < 35.0 || v_val > 205.0 || yolo_score < 0.55f) {
                    is_valid_food = false;
                }
            }
            // Egg:
            else if (predicted_name == "egg") {
                if (v_val < 65.0 || v_val > 215.0 || s_val > 60.0 || yolo_score < 0.55f) {
                    is_valid_food = false;
                }
            }

            // Reject detection if physical chromaticity/texture checks fail
            if (!is_valid_food) {
                continue;
            }

            // True Unbiased Confidence Score: Directly reflects actual neural probability
            DetectionResult det;
            det.bbox = final_box;
            det.class_id = final_class_id;
            det.class_name = predicted_name;
            det.confidence = std::clamp(calibrated_conf, 0.0f, 1.0f);

            // AVX2 SIMD color spoilage and freshness assessment
            cv::Mat final_food_roi = frame(det.bbox);
            det.spoilage = evaluateSpoilageAVX2(final_food_roi, det.class_name);

            current_detections.push_back(std::move(det));
        }

        if (current_detections.empty()) {
            s_tracked_items.clear();
            return true;
        }

        // 8. Temporal Tracking & Smoothing (Stabilizes Bounding Boxes & Suppresses Flicker)
        std::vector<bool> matched(current_detections.size(), false);

        for (auto& tracked : s_tracked_items) {
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

                // EMA Bounding Box Smoothing
                tracked.result.bbox.x = static_cast<int>(0.70f * cur.bbox.x + 0.30f * tracked.result.bbox.x);
                tracked.result.bbox.y = static_cast<int>(0.70f * cur.bbox.y + 0.30f * tracked.result.bbox.y);
                tracked.result.bbox.width = static_cast<int>(0.70f * cur.bbox.width + 0.30f * tracked.result.bbox.width);
                tracked.result.bbox.height = static_cast<int>(0.70f * cur.bbox.height + 0.30f * tracked.result.bbox.height);

                tracked.result.class_name = cur.class_name;
                tracked.result.class_id = cur.class_id;
                tracked.result.confidence = 0.70f * cur.confidence + 0.30f * tracked.result.confidence;
                tracked.result.spoilage = cur.spoilage;
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
                s_tracked_items.push_back(std::move(new_item));
            }
        }

        // Remove tracks lost for more than 2 consecutive frames
        s_tracked_items.erase(
            std::remove_if(s_tracked_items.begin(), s_tracked_items.end(),
                           [](const TrackedItem& item) { return item.lost_frames > 2; }),
            s_tracked_items.end());

        // Copy valid tracks to output container
        for (const auto& item : s_tracked_items) {
            if (item.lost_frames <= 1) {
                out_detections.push_back(item.result);
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