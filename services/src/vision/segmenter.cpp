#include "bawal/vision/segmenter.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <vector>
#include <cmath>

namespace bawal::vision {

bool Segmenter::initStandardTrayLayout(int tray_width, int tray_height) {
    if (tray_width <= 0 || tray_height <= 0) return false;
    m_compartments.clear();
    int half_w = tray_width / 2;
    int half_h = tray_height / 2;

    m_compartments.push_back({0, "food_item_1", cv::Rect(0, 0, half_w, tray_height), cv::Mat()});
    m_compartments.push_back({1, "food_item_2", cv::Rect(half_w, 0, half_w, half_h), cv::Mat()});
    m_compartments.push_back({2, "food_item_3", cv::Rect(half_w, half_h, half_w / 2, half_h), cv::Mat()});
    m_compartments.push_back({3, "food_item_4", cv::Rect(half_w + (half_w / 2), half_h, half_w / 2, half_h), cv::Mat()});
    return true;
}

bool Segmenter::extractCompartments(const cv::Mat& dewarped_frame, 
                                    std::vector<TrayCompartment>& out_compartments) {
    out_compartments.clear();
    if (dewarped_frame.empty()) {
        return false;
    }

    const int frame_w = dewarped_frame.cols;
    const int frame_h = dewarped_frame.rows;
    const double total_area = static_cast<double>(frame_w * frame_h);

    cv::Mat gray, blurred;
    cv::cvtColor(dewarped_frame, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    // 1. Lab Color Space Salience Extraction (L: Luminance, a & b: Chrominance)
    cv::Mat lab;
    cv::cvtColor(dewarped_frame, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> lab_planes;
    cv::split(lab, lab_planes);

    // 2. Chromatic Object Detection (Dishes, Vegetables, Proteins)
    cv::Mat a_diff, b_diff, chrom_mask;
    cv::absdiff(lab_planes[1], cv::Scalar(128), a_diff);
    cv::absdiff(lab_planes[2], cv::Scalar(128), b_diff);
    cv::Mat chromatic = a_diff + b_diff;
    cv::threshold(chromatic, chrom_mask, 16, 255, cv::THRESH_BINARY);

    // 3. Bright Object & Rice Detection (High Luminance L >= 135)
    cv::Mat bright_mask;
    cv::threshold(lab_planes[0], bright_mask, 135, 255, cv::THRESH_BINARY);

    // 4. Physical Contour & Edge Detection
    cv::Mat morph_grad;
    cv::Mat grad_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(blurred, morph_grad, cv::MORPH_GRADIENT, grad_kernel);
    cv::Mat edge_mask;
    cv::threshold(morph_grad, edge_mask, 18, 255, cv::THRESH_BINARY);

    // 5. Combine Candidate Food & Container Masks
    cv::Mat combined_mask;
    cv::bitwise_or(chrom_mask, bright_mask, combined_mask);
    cv::bitwise_or(combined_mask, edge_mask, combined_mask);

    // 6. Morphological Closing & Opening
    cv::Mat kernel_close = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(19, 19));
    cv::morphologyEx(combined_mask, combined_mask, cv::MORPH_CLOSE, kernel_close);

    cv::Mat kernel_open = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
    cv::morphologyEx(combined_mask, combined_mask, cv::MORPH_OPEN, kernel_open);

    // 7. Find External Contours (Murni Sekuensial - Bebas Data Race)
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(combined_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const double min_area = std::max(1000.0, total_area * 0.008);
    const double max_area = total_area * 0.75;

    std::vector<cv::Rect> candidate_boxes;
    for (const auto& cnt : contours) {
        double area = cv::contourArea(cnt);
        if (area >= min_area && area <= max_area) {
            std::vector<cv::Point> hull;
            cv::convexHull(cnt, hull);
            cv::Rect br = cv::boundingRect(hull);

            int pad = 8;
            int x = std::max(0, br.x - pad);
            int y = std::max(0, br.y - pad);
            int w = std::min(frame_w - x, br.width + 2 * pad);
            int h = std::min(frame_h - y, br.height + 2 * pad);

            float aspect_ratio = static_cast<float>(w) / static_cast<float>(h);
            if (aspect_ratio >= 0.25f && aspect_ratio <= 4.0f) {
                candidate_boxes.emplace_back(x, y, w, h);
            }
        }
    }

    std::vector<cv::Rect> dynamic_boxes;
    for (const auto& box : candidate_boxes) {
        bool merged = false;
        for (auto& existing : dynamic_boxes) {
            cv::Rect inter = box & existing;
            if (inter.area() > 0) {
                float iou = static_cast<float>(inter.area()) / static_cast<float>((box | existing).area());
                if (iou > 0.10f || inter.area() > 0.30f * std::min(box.area(), existing.area())) {
                    existing = box | existing;
                    merged = true;
                    break;
                }
            }
        }
        if (!merged) {
            dynamic_boxes.push_back(box);
        }
    }

    if (dynamic_boxes.size() > 6) {
        std::sort(dynamic_boxes.begin(), dynamic_boxes.end(), [](const cv::Rect& a, const cv::Rect& b) {
            return a.area() > b.area();
        });
        dynamic_boxes.resize(6);
    }

    const int num_rois = static_cast<int>(dynamic_boxes.size());
    out_compartments.clear();
    out_compartments.reserve(num_rois);

    for (int i = 0; i < num_rois; ++i) {
        TrayCompartment comp;
        comp.id = i;
        comp.name = "food_item_" + std::to_string(i + 1);
        comp.roi = dynamic_boxes[i];
        comp.cropped_mat = dewarped_frame(dynamic_boxes[i]).clone();
        out_compartments.push_back(std::move(comp));
    }

    return true;
}

} // namespace bawal::vision