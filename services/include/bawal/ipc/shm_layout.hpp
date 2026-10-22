#pragma once
#include <cstdint>
#include <atomic>

namespace bawal::ipc {

constexpr uint32_t SHM_MAGIC = 0x4241574C; // 'BAWL'
constexpr uint32_t SHM_NUM_SLOTS = 4;
constexpr uint32_t SHM_SLOT_CAPACITY = 1024 * 1024; // 1 MB per frame slot
constexpr const char* SHM_NAME = "/bawal_frame_shm";
// Keep the notify socket in /dev/shm so the backend and vision containers can
// share it when both run with `ipc: host`.
constexpr const char* SOCK_NOTIFY_PATH = "/dev/shm/bawal_shm.sock";

// Global Header: Aligned ke 64 byte (1 Cache line)
struct alignas(64) ShmGlobalHeader {
    uint32_t magic{SHM_MAGIC};
    uint32_t version{1};
    uint32_t num_slots{SHM_NUM_SLOTS};
    uint32_t slot_capacity{SHM_SLOT_CAPACITY};
    std::atomic<uint64_t> write_seq{0}; // Nomor urut frame global terbaru
    uint8_t reserved[40]{0};            // Explicit padding to 64 bytes
};
static_assert(sizeof(ShmGlobalHeader) == 64, "Header must be 64 bytes");

// Metadata Per Slot: Aligned ke 64 byte (1 Cache line)
struct alignas(64) ShmSlotHeader {
    std::atomic<uint32_t> ready{0}; // 0 = idle/writing, 1 = ready for read
    uint32_t frame_id{0};
    uint64_t timestamp_ns{0};
    uint32_t payload_size{0};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t  format{0};             // 0 = JPEG, 1 = BGR, 2 = YUV
    uint8_t  reserved[39]{0};       // Explicit padding to 64 bytes
};
static_assert(sizeof(ShmSlotHeader) == 64, "SlotHeader must be 64 bytes");

constexpr size_t SLOT_STRIDE = sizeof(ShmSlotHeader) + SHM_SLOT_CAPACITY;
constexpr size_t TOTAL_SHM_SIZE = sizeof(ShmGlobalHeader) + (SHM_NUM_SLOTS * SLOT_STRIDE);

} // namespace bawal::ipc