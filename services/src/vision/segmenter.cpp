#include "bawal/vision/segmenter.hpp"
namespace bawal::vision {

bool Segmenter::initStandardTrayLayout(int tray_width, int tray_height) {
    if (tray_width <= 0 || tray_height <= 0) return false;

    m_compartments.clear();
    // Standard MBG kitchen 4-compartment meal tray layout:
    // [Main Carb / Rice (Large Left)] | [Vegetables (Top Right)]
    // [Protein / Chicken (Center Left)] | [Fruit / Side (Bottom Right)]
    
    int half_w = tray_width / 2;
    int half_h = tray_height / 2;

    m_compartments.push_back({0, "main_carb", cv::Rect(0, 0, half_w, tray_height), cv::Mat()});
    m_compartments.push_back({1, "veggie", cv::Rect(half_w, 0, half_w, half_h), cv::Mat()});
    m_compartments.push_back({2, "protein", cv::Rect(half_w, half_h, half_w / 2, half_h), cv::Mat()});
    m_compartments.push_back({3, "fruit_side", cv::Rect(half_w + (half_w / 2), half_h, half_w / 2, half_h), cv::Mat()});

    return true;
}

bool Segmenter::extractCompartments(const cv::Mat& dewarped_frame, 
                                    std::vector<TrayCompartment>& out_compartments) {
    if (dewarped_frame.empty() || m_compartments.empty()) {
        return false;
    }

    out_compartments = m_compartments;
    const cv::Rect frame_bounds(0, 0, dewarped_frame.cols, dewarped_frame.rows);

    const int num_comps = static_cast<int>(out_compartments.size());
    bool all_valid = true;

    // OpenMP Parallelization across tray compartments
    #pragma omp parallel for schedule(static) shared(out_compartments, all_valid)
    for (int i = 0; i < num_comps; ++i) {
        if (!all_valid) continue;
        auto& comp = out_compartments[i];
        const cv::Rect valid_roi = comp.roi & frame_bounds;
        if (valid_roi.area() > 0) {
            comp.cropped_mat = dewarped_frame(valid_roi).clone();
        } else {
            #pragma omp atomic write
            all_valid = false;
        }
    }

    return all_valid;
}

} // namespace bawal::vision