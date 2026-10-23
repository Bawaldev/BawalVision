#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::backend {

/** Host USB serial port discovered under /dev (ttyACM* or ttyUSB*). */
struct alignas(8) UsbPortInfo {
    std::string port;
    std::string name;
    std::string vendor_id;
    std::string product_id;
    std::string manufacturer;
    std::string product;
    bool is_esp32{false};
    uint8_t reserved[7]{0}; ///< Explicit tail padding keeps the layout 8-byte aligned
};

/** Enumerate `/dev/ttyACM*` and `/dev/ttyUSB*` with their sysfs metadata. */
[[nodiscard]] std::vector<UsbPortInfo> scanUsbSerialPorts();

/** True for USB vendor IDs known to ship ESP32-S3 boards. */
[[nodiscard]] bool isEspUsbVendor(std::string_view vendor_id);

} // namespace bawal::backend
