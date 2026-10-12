#include "bawal/vision/video_source.hpp"
#include <opencv2/videoio.hpp>

#include <string>
#include <span>

namespace bawal::vision {

bool VideoSource::open(SourceMode new_mode, const std::string& uri) {
    m_mode = new_mode;
    if (m_mode == SourceMode::LOCAL_MP4 || m_mode == SourceMode::WEBCAM_USB) {
        return m_cap.open(uri);
    }
    return true;
}

bool VideoSource::grabNextFrame(cv::Mat& out_frame) {
    if (m_mode == SourceMode::LOCAL_MP4) {
        if (!m_cap.read(out_frame) || out_frame.empty()) {
            m_cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            return m_cap.read(out_frame);
        }
        return true;
    }
    return m_cap.read(out_frame);
}



}