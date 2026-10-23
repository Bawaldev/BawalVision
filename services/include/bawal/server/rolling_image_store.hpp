#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

namespace bawal::server {

class RollingImageStore {
public:
    explicit RollingImageStore(std::filesystem::path directory = "./data/inspections",
                               std::size_t capacity = 50);

    [[nodiscard]] std::string save_frame(uint64_t frame_id, const cv::Mat& bgr_mat);
    [[nodiscard]] std::string save_encoded(uint64_t frame_id,
                                           const std::vector<uint8_t>& jpeg_bytes);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::filesystem::path directory() const { return m_directory; }

private:
    [[nodiscard]] std::string persist_locked(uint64_t frame_id,
                                             const std::vector<uint8_t>& jpeg_bytes);

    std::filesystem::path m_directory;
    std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::deque<std::string> m_queue;
};

} // namespace bawal::server
