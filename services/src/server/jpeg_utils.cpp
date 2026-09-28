#include <bawal/server/jpeg_utils.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iterator>

namespace bawal::server {

bool decodeMJPEGFrame(const std::vector<uint8_t>& raw_bytes, cv::Mat& out_frame) {
    out_frame.release();
    if (raw_bytes.empty()) {
        return false;
    }

    // Step 1: Direct OpenCV decode attempt
    out_frame = cv::imdecode(raw_bytes, cv::IMREAD_COLOR);
    if (!out_frame.empty()) {
        return true;
    }

    // Step 2: Search for SOI marker (0xFF, 0xD8) to strip any leading transport bytes
    bool found_soi = false;
    size_t soi = 0;
    const size_t byte_count = raw_bytes.size();

    for (size_t i = 0; i + 1 < byte_count; ++i) {
        if (raw_bytes[i] == 0xFF && raw_bytes[i + 1] == 0xD8) {
            soi = i;
            found_soi = true;
            break;
        }
    }

    if (found_soi && soi > 0) {
        std::vector<uint8_t> aligned(raw_bytes.begin() + soi, raw_bytes.end());
        out_frame = cv::imdecode(aligned, cv::IMREAD_COLOR);
        if (!out_frame.empty()) {
            return true;
        }
    }

    // Step 3: Check if DHT table (0xFF, 0xC4) is missing and inject standard JPEG DHT table
    const uint8_t* data = raw_bytes.data() + (found_soi ? soi : 0);
    const size_t len = raw_bytes.size() - (found_soi ? soi : 0);

    if (len > 4 && data[0] == 0xFF && data[1] == 0xD8) {
        bool has_dht = false;
        for (size_t i = 0; i + 1 < len; ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0xC4) {
                has_dht = true;
                break;
            }
            if (data[i] == 0xFF && data[i + 1] == 0xDA) { // SOS marker
                break;
            }
        }

        if (!has_dht) {
            std::vector<uint8_t> reconstructed;
            reconstructed.reserve(len + sizeof(K_STANDARD_DHT));
            reconstructed.insert(reconstructed.end(), data, data + 2);
            reconstructed.insert(reconstructed.end(), std::begin(K_STANDARD_DHT), std::end(K_STANDARD_DHT));
            reconstructed.insert(reconstructed.end(), data + 2, data + len);

            out_frame = cv::imdecode(reconstructed, cv::IMREAD_COLOR);
            if (!out_frame.empty()) {
                return true;
            }
        }
    }

    // Step 4: Fallback for raw uncompressed YUYV (YUV422) camera streams
    int width = 0;
    int height = 0;

    if (byte_count == 160 * 120 * 2) {
        width = 160;
        height = 120;
    } else if (byte_count == 320 * 240 * 2) {
        width = 320;
        height = 240;
    } else if (byte_count == 640 * 360 * 2) {
        width = 640;
        height = 360;
    } else if (byte_count == 640 * 480 * 2) {
        width = 640;
        height = 480;
    } else if (byte_count == 1280 * 720 * 2) {
        width = 1280;
        height = 720;
    }

    if (width > 0 && height > 0) {
        try {
            cv::Mat yuv(height, width, CV_8UC2, const_cast<uint8_t*>(raw_bytes.data()));
            cv::cvtColor(yuv, out_frame, cv::COLOR_YUV2BGR_YUYV);
            if (!out_frame.empty()) {
                return true;
            }
        } catch (...) {
            // Fallthrough to failure
        }
    }

    out_frame.release();
    return false;
}

} // namespace bawal::server
