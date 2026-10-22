#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <type_traits>

namespace bawal::ipc {

constexpr uint32_t SHM_MAGIC = 0x4241574C; // 'BAWL'
constexpr uint32_t SHM_NUM_SLOTS = 4;
// ESP32-S3 frames are capped at 256 KB; 512 KB gives headroom while keeping
// the shared mapping small. Must match slotCapacity in backend/internal/shm/producer.go.
constexpr uint32_t SHM_SLOT_CAPACITY = 512 * 1024; // 512 KB per frame slot
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

// The payload of every slot must start on a cache-line boundary. Keeping the
// ring stride a multiple of 64 also means the slot headers are never split
// across cache lines, which matters for the producer/consumer seqlock.
constexpr size_t SLOT_STRIDE = sizeof(ShmSlotHeader) + SHM_SLOT_CAPACITY;
constexpr size_t TOTAL_SHM_SIZE = sizeof(ShmGlobalHeader) + (SHM_NUM_SLOTS * SLOT_STRIDE);

// Layout checks shared with backend/internal/shm/producer.go. The Go producer
// writes these fields at hard-coded byte offsets, so any layout drift must be
// caught at compile time rather than silently corrupting frames at runtime.
static_assert(std::is_standard_layout_v<ShmGlobalHeader>);
static_assert(std::is_standard_layout_v<ShmSlotHeader>);
static_assert(offsetof(ShmGlobalHeader, magic) == 0);
static_assert(offsetof(ShmGlobalHeader, version) == 4);
static_assert(offsetof(ShmGlobalHeader, num_slots) == 8);
static_assert(offsetof(ShmGlobalHeader, slot_capacity) == 12);
static_assert(offsetof(ShmGlobalHeader, write_seq) == 16);
static_assert(offsetof(ShmSlotHeader, ready) == 0);
static_assert(offsetof(ShmSlotHeader, frame_id) == 4);
static_assert(offsetof(ShmSlotHeader, timestamp_ns) == 8);
static_assert(offsetof(ShmSlotHeader, payload_size) == 16);
static_assert(offsetof(ShmSlotHeader, width) == 20);
static_assert(offsetof(ShmSlotHeader, height) == 22);
static_assert(offsetof(ShmSlotHeader, format) == 24);
static_assert(SLOT_STRIDE % 64 == 0, "Slot payload must start on a 64-byte boundary");
static_assert(SHM_NUM_SLOTS == 4 && (SHM_NUM_SLOTS & (SHM_NUM_SLOTS - 1)) == 0,
              "Ring size must be a power of two");
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace bawal::ipc