#include "bawal/ipc/shm_consumer.hpp"

#include <cerrno>
#include <sys/time.h>

namespace bawal::ipc {

bool ShmFrameConsumer::init() {
    m_shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (m_shm_fd < 0) {
        std::cerr << "[SHM] shm_open failed: " << strerror(errno) << std::endl;
        return false;
    }
    void* ptr = mmap(nullptr, TOTAL_SHM_SIZE, PROT_READ, MAP_SHARED, m_shm_fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "[SHM] mmap failed: " << strerror(errno) << std::endl;
        return false;
    }
    m_base_ptr = reinterpret_cast<uint8_t*>(ptr);
    m_global_hdr = reinterpret_cast<ShmGlobalHeader*>(m_base_ptr);
    if (m_global_hdr->magic != SHM_MAGIC) {
        std::cerr << "[SHM] Magic header mismatch (backend not ready?)" << std::endl;
        return false;
    }

    unlink(SOCK_NOTIFY_PATH);
    m_server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_server_sock < 0) {
        std::cerr << "[SHM] Failed to create UDS notify socket: " << strerror(errno) << std::endl;
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_NOTIFY_PATH, sizeof(addr.sun_path) - 1);

    if (bind(m_server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[SHM] Failed to bind UDS notify socket: " << strerror(errno) << std::endl;
        return false;
    }
    // Backend and vision may run as different users; allow the Go producer to
    // connect to the notify socket regardless of umask.
    chmod(SOCK_NOTIFY_PATH, 0666);

    // A bounded accept/read makes waitForNextFrame() pollable. Without this,
    // shutdown would have to close a socket owned by another thread while the
    // consumer is blocked inside accept() or read().
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200 * 1000;
    (void)setsockopt(m_server_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    listen(m_server_sock, 1);
    std::cout << "[SHM] Consumer ready. Waiting for the producer to connect...\n";
    return true;
}

bool ShmFrameConsumer::waitForNextFrame(cv::Mat& out_bgr_frame, uint32_t& out_frame_id) {
    if (m_client_sock < 0) {
        m_client_sock = accept(m_server_sock, nullptr, nullptr);
        if (m_client_sock < 0) return false;

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;
        (void)setsockopt(m_client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::cout << "[SHM] Producer connected to the C++ pipeline!\n";
    }

    uint64_t latest_seq = 0;
    ssize_t n = read(m_client_sock, &latest_seq, sizeof(latest_seq));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A notification may have been dropped (producer restart or a
            // full socket buffer). Fall back to the authoritative global
            // sequence so the freshest frame is still picked up.
            latest_seq = m_global_hdr->write_seq.load(std::memory_order_acquire);
            if (latest_seq == 0 || latest_seq <= m_last_seq) {
                return false;
            }
        } else {
            close(m_client_sock);
            m_client_sock = -1;
            return false;
        }
    } else {
        // Drain every queued notification and process only the freshest sequence.
        // Without this, a slow consumer would replay old tokens and build latency.
        uint64_t queued_seq = 0;
        while (recv(m_client_sock, &queued_seq, sizeof(queued_seq), MSG_DONTWAIT) == sizeof(queued_seq)) {
            latest_seq = queued_seq;
        }

        uint64_t global_seq = m_global_hdr->write_seq.load(std::memory_order_acquire);
        if (global_seq > latest_seq) {
            latest_seq = global_seq;
        }
        if (latest_seq == 0 || latest_seq <= m_last_seq) {
            return false;
        }
    }

    uint32_t slot_idx = (latest_seq - 1) & (SHM_NUM_SLOTS - 1);
    size_t slot_offset = sizeof(ShmGlobalHeader) + (slot_idx * SLOT_STRIDE);

    auto* slot_hdr = reinterpret_cast<ShmSlotHeader*>(m_base_ptr + slot_offset);
    const uint8_t* payload_ptr = m_base_ptr + slot_offset + sizeof(ShmSlotHeader);

    if (slot_hdr->ready.load(std::memory_order_acquire) != 1) {
        return false;
    }

    uint32_t size = slot_hdr->payload_size;
    if (size == 0 || size > SHM_SLOT_CAPACITY) {
        return false;
    }

    const uint32_t frame_id_before = slot_hdr->frame_id;
    out_frame_id = frame_id_before;
    cv::Mat raw_jpeg_view(1, static_cast<int>(size), CV_8UC1, const_cast<uint8_t*>(payload_ptr));
    cv::imdecode(raw_jpeg_view, cv::IMREAD_COLOR, &out_bgr_frame);

    // Seqlock-style revalidation: if the producer wrapped onto this slot while
    // we were decoding it, discard the (possibly torn) result instead of
    // feeding corrupt pixels to the pipeline.
    if (slot_hdr->ready.load(std::memory_order_acquire) != 1 ||
        slot_hdr->frame_id != frame_id_before) {
        out_bgr_frame.release();
        return false;
    }

    if (out_bgr_frame.empty()) {
        return false;
    }

    m_last_seq = latest_seq;
    return true;
}

void ShmFrameConsumer::cleanup() {
    if (m_client_sock >= 0) close(m_client_sock);
    if (m_server_sock >= 0) {
        close(m_server_sock);
        unlink(SOCK_NOTIFY_PATH);
    }
    if (m_base_ptr != nullptr) munmap(m_base_ptr, TOTAL_SHM_SIZE);
    if (m_shm_fd >= 0) close(m_shm_fd);
}


};