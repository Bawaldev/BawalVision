#include "bawal/server/rolling_image_store.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>

#include "bawal/server/jpeg_utils.hpp"

namespace bawal::server {

RollingImageStore::RollingImageStore(std::filesystem::path directory, std::size_t capacity)
    : m_directory(std::move(directory)), m_capacity(capacity == 0 ? 1 : capacity) {
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec) {
        std::cerr << "[IMAGE-STORE] Unable to create " << m_directory.string()
                  << ": " << ec.message() << '\n';
    }
}

std::string RollingImageStore::save_frame(uint64_t frame_id, const cv::Mat& bgr_mat) {
    if (bgr_mat.empty()) {
        return {};
    }

    std::vector<uint8_t> jpeg;
    if (!encodeJPEGFrame(bgr_mat, jpeg, 82)) {
        return {};
    }
    return save_encoded(frame_id, jpeg);
}

std::string RollingImageStore::save_encoded(uint64_t frame_id,
                                            const std::vector<uint8_t>& jpeg_bytes) {
    if (jpeg_bytes.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string filename = persist_locked(frame_id, jpeg_bytes);
    if (filename.empty()) {
        return {};
    }
    m_queue.push_back(filename);
    return "/inspections/" + filename;
}

std::size_t RollingImageStore::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

std::string RollingImageStore::persist_locked(uint64_t frame_id,
                                              const std::vector<uint8_t>& jpeg_bytes) {
    while (m_queue.size() >= m_capacity && !m_queue.empty()) {
        const std::filesystem::path oldest = m_directory / m_queue.front();
        std::error_code ec;
        std::filesystem::remove(oldest, ec);
        m_queue.pop_front();
    }

    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string filename =
        "insp_" + std::to_string(frame_id) + "_" + std::to_string(now) + ".jpg";
    std::filesystem::path path = m_directory / filename;

    std::error_code exists_ec;
    int suffix = 0;
    while (std::filesystem::exists(path, exists_ec)) {
        filename = "insp_" + std::to_string(frame_id) + "_" + std::to_string(now) +
                   "_" + std::to_string(++suffix) + ".jpg";
        path = m_directory / filename;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "[IMAGE-STORE] Unable to open " << path.string() << '\n';
        return {};
    }
    output.write(reinterpret_cast<const char*>(jpeg_bytes.data()),
                 static_cast<std::streamsize>(jpeg_bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "[IMAGE-STORE] Failed to write " << path.string() << '\n';
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        return {};
    }
    return filename;
}

} // namespace bawal::server
