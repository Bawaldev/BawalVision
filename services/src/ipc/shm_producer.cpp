#include "bawal/ipc/shm_producer.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace bawal::ipc {

bool ShmProducer::init() {
    closeMappings();

    m_shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (m_shm_fd < 0) {
        return false;
    }

    if (ftruncate(m_shm_fd, static_cast<off_t>(TOTAL_SHM_SIZE)) != 0) {
        ::close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }
    (void)fchmod(m_shm_fd, 0666);

    void* mapped = mmap(nullptr, TOTAL_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }

    m_base_ptr = reinterpret_cast<uint8_t*>(mapped);
    m_global_hdr = reinterpret_cast<ShmGlobalHeader*>(m_base_ptr);

    // Fresh mapping: construct every cache-line-aligned header. This also
    // clears slots left behind by a previous process, so no stale frame can be
    // consumed before the first HTTP upload of this run.
    new (m_global_hdr) ShmGlobalHeader{};
    for (uint32_t i = 0; i < SHM_NUM_SLOTS; ++i) {
        uint8_t* slot_ptr = m_base_ptr + sizeof(ShmGlobalHeader) + (i * SLOT_STRIDE);
        new (slot_ptr) ShmSlotHeader{};
    }

    m_write_seq.store(0, std::memory_order_release);
    return true;
}

bool ShmProducer::writeFrame(uint32_t frame_id, const std::vector<uint8_t>& data,
                             uint16_t width, uint16_t height, uint8_t format) {
    return writeFrame(frame_id, data.data(), data.size(), width, height, format);
}

bool ShmProducer::writeFrame(uint32_t frame_id, const uint8_t* data, size_t len,
                             uint16_t width, uint16_t height, uint8_t format) {
    if (!ready() || data == nullptr || len == 0 || len > SHM_SLOT_CAPACITY) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_write_mutex);

    const uint64_t seq = m_write_seq.load(std::memory_order_relaxed);
    const uint32_t slot_idx = static_cast<uint32_t>(seq) & (SHM_NUM_SLOTS - 1);
    const size_t slot_offset = sizeof(ShmGlobalHeader) + (slot_idx * SLOT_STRIDE);

    auto* slot = reinterpret_cast<ShmSlotHeader*>(m_base_ptr + slot_offset);
    uint8_t* payload = m_base_ptr + slot_offset + sizeof(ShmSlotHeader);

    // Seqlock write side. Mark the slot busy before exposing any new bytes.
    slot->ready.store(0, std::memory_order_release);
    std::memcpy(payload, data, len);

    slot->frame_id = frame_id;
    slot->timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    slot->payload_size = static_cast<uint32_t>(len);
    slot->width = width;
    slot->height = height;
    slot->format = format;
    slot->ready.store(1, std::memory_order_release);

    const uint64_t next_seq = seq + 1;
    m_global_hdr->write_seq.store(next_seq, std::memory_order_release);
    m_write_seq.store(next_seq, std::memory_order_release);

    const int fd = m_notify_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const ssize_t sent = send(fd, &next_seq, sizeof(next_seq),
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent != static_cast<ssize_t>(sizeof(next_seq))) {
            // The consumer restarted or its socket buffer is full. The global
            // sequence is still authoritative, so dropping this token is safe.
            int expected = fd;
            if (m_notify_fd.compare_exchange_strong(expected, -1,
                                                    std::memory_order_acq_rel)) {
                ::close(fd);
            }
        }
    }

    return true;
}

void ShmProducer::startNotify() {
    if (m_notify_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_notify_thread = std::thread(&ShmProducer::notifyLoop, this);
}

void ShmProducer::stopNotify() {
    m_notify_running.store(false, std::memory_order_release);

    const int fd = m_notify_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    if (m_notify_thread.joinable()) {
        m_notify_thread.join();
    }
}

void ShmProducer::notifyLoop() {
    while (m_notify_running.load(std::memory_order_acquire)) {
        const int current = m_notify_fd.load(std::memory_order_acquire);
        if (current >= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, SOCK_NOTIFY_PATH, sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int expected = -1;
        if (!m_notify_fd.compare_exchange_strong(expected, fd,
                                                 std::memory_order_acq_rel)) {
            ::close(fd);
        }
    }
}

void ShmProducer::closeMappings() {
    stopNotify();
    if (m_base_ptr != nullptr) {
        munmap(m_base_ptr, TOTAL_SHM_SIZE);
        m_base_ptr = nullptr;
        m_global_hdr = nullptr;
    }
    if (m_shm_fd >= 0) {
        ::close(m_shm_fd);
        m_shm_fd = -1;
    }
}

ShmProducer::~ShmProducer() {
    closeMappings();
}

} // namespace bawal::ipc
