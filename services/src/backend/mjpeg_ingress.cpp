#include "bawal/backend/mjpeg_ingress.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string_view>

#include <curl/curl.h>

#include "bawal/backend/state.hpp"
#include "bawal/ipc/shm_producer.hpp"

namespace bawal::backend {
namespace {

struct TransferContext {
    MjpegIngress* ingress;
    uint64_t generation;
    std::string pending;
};

size_t parseContentLength(std::string_view header) {
    constexpr std::string_view key = "Content-Length:";
    size_t pos = header.find(key);
    if (pos == std::string_view::npos) return 0;
    pos += key.size();
    while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos;
    size_t end = pos;
    while (end < header.size() && header[end] >= '0' && header[end] <= '9') ++end;
    if (end == pos) return 0;
    try {
        return static_cast<size_t>(std::stoull(std::string(header.substr(pos, end - pos))));
    } catch (...) {
        return 0;
    }
}

size_t receiveMjpeg(char* data, size_t size, size_t count, void* user) {
    const size_t bytes = size * count;
    auto& ctx = *static_cast<TransferContext*>(user);
    constexpr size_t kMaxPendingBytes = 512 * 1024;
    if (bytes == 0 || ctx.pending.size() + bytes > kMaxPendingBytes) return 0;
    ctx.pending.append(data, bytes);

    for (;;) {
        const size_t header_end = ctx.pending.find("\r\n\r\n");
        if (header_end == std::string::npos) break;
        const size_t payload_len = parseContentLength(
            std::string_view(ctx.pending.data(), header_end));
        if (payload_len == 0 || payload_len > 256 * 1024) return 0;
        const size_t payload_start = header_end + 4;
        if (ctx.pending.size() < payload_start + payload_len + 2) break;

        ctx.ingress->publishFrame(
            reinterpret_cast<const uint8_t*>(ctx.pending.data() + payload_start), payload_len);
        size_t consumed = payload_start + payload_len;
        if (ctx.pending.compare(consumed, 2, "\r\n") == 0) consumed += 2;
        ctx.pending.erase(0, consumed);
    }
    return bytes;
}

int transferProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& ctx = *static_cast<TransferContext*>(user);
    return ctx.ingress->shouldStop(ctx.generation) ? 1 : 0;
}

} // namespace

MjpegIngress::MjpegIngress(BackendState& state, ipc::ShmProducer& producer)
    : m_state(state), m_producer(producer) {}

MjpegIngress::~MjpegIngress() { stop(); }

void MjpegIngress::start() {
    bool expected = false;
    if (m_running.compare_exchange_strong(expected, true)) {
        m_thread = std::thread(&MjpegIngress::run, this);
    }
}

void MjpegIngress::stop() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void MjpegIngress::setPeerIp(std::string ip) {
    {
        std::lock_guard lock(m_mutex);
        if (m_peer_ip == ip) return;
        m_peer_ip = std::move(ip);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    m_cv.notify_all();
}

bool MjpegIngress::shouldStop(uint64_t generation) const {
    return !m_running.load(std::memory_order_acquire) ||
           m_generation.load(std::memory_order_acquire) != generation;
}

void MjpegIngress::publishFrame(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > 256 * 1024) return;
    const uint32_t id = m_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m_producer.writeFrame(id, data, len, 640, 480)) {
        m_state.updateFrame("ESP32_CAMERA", id, std::vector<uint8_t>(data, data + len), 640, 480);
    }
}

void MjpegIngress::run() {
    while (m_running.load(std::memory_order_acquire)) {
        std::string peer;
        uint64_t generation = m_generation.load(std::memory_order_acquire);
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::seconds(1), [this] {
                return !m_running.load(std::memory_order_acquire) || !m_peer_ip.empty();
            });
            if (!m_running.load(std::memory_order_acquire)) break;
            peer = m_peer_ip;
            generation = m_generation.load(std::memory_order_acquire);
        }
        if (peer.empty()) continue;

        CURL* curl = curl_easy_init();
        if (!curl) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        TransferContext context{this, generation, {}};
        const std::string url = "http://" + peer + "/stream";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveMjpeg);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        const CURLcode result = curl_easy_perform(curl);
        if (result != CURLE_OK && !shouldStop(generation)) {
            std::cerr << "[WARN] MJPEG pull from " << peer << " stopped: "
                      << curl_easy_strerror(result) << '\n';
        }
        curl_easy_cleanup(curl);
        if (m_running.load(std::memory_order_acquire) &&
            m_generation.load(std::memory_order_acquire) == generation) {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this, generation] {
                return !m_running.load(std::memory_order_acquire) ||
                       m_generation.load(std::memory_order_acquire) != generation;
            });
        }
    }
}

} // namespace bawal::backend
