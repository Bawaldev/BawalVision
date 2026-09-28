#pragma once

#include <opencv2/core.hpp>
#include <vector>
#include <string>

namespace bawal::vision {

struct TrayCompartment {
    int id{0};
    std::string name;
    cv::Rect roi;
    cv::Mat cropped_mat;
};

class Segmenter {
public:
    Segmenter() = default;
    ~Segmenter() = default;

    /**
     * @brief Configure standard MBG meal tray compartment layout.
     * 
     * @param tray_width Width of tray area after dewarping.
     * @param tray_height Height of tray area after dewarping.
     * @return true if compartment layout is valid.
     */
    [[nodiscard]] bool initStandardTrayLayout(int tray_width, int tray_height);

    /**
     * @brief Extract cropped ROI matrices for each meal compartment in parallel.
     * 
     * @param dewarped_frame Dewarped source matrix.
     * @param out_compartments Vector to store cropped sub-images for each food item.
     * @return true if compartments were extracted successfully without dimension errors.
     */
    [[nodiscard]] bool extractCompartments(const cv::Mat& dewarped_frame, 
                                           std::vector<TrayCompartment>& out_compartments);

    /**
     * @brief Get total number of registered compartments.
     * 
     * @return Total compartment count.
     */
    [[nodiscard]] size_t getCompartmentCount() const noexcept {
        return m_compartments.size();
    }

private:
    std::vector<TrayCompartment> m_compartments;
};

} // namespace bawal::vision