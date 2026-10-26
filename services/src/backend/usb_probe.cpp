#include "bawal/backend/usb_probe.hpp"

#include <algorithm>

#include <filesystem>
#include <fstream>

#include "bawal/backend/text_utils.hpp"

namespace {

std::string readSysfsValue(const std::string& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return bawal::backend::trimCopy(value);
}

} // namespace

namespace bawal::backend {

bool isEspUsbVendor(std::string_view vendor_id) {
    return vendor_id == "303a" || vendor_id == "10c4" ||
           vendor_id == "1a86" || vendor_id == "0403";
}

std::vector<UsbPortInfo> scanUsbSerialPorts() {
    std::vector<UsbPortInfo> ports;
    std::error_code ec;
    std::filesystem::directory_iterator it("/dev", ec);
    if (ec) {
        return ports;
    }

    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string node = it->path().filename().string();
        if (node.rfind("ttyACM", 0) != 0 && node.rfind("ttyUSB", 0) != 0) {
            continue;
        }

        UsbPortInfo info;
        info.port = "/dev/" + node;
        info.name = node.rfind("ttyACM", 0) == 0 ? "ESP32-S3 USB Serial/JTAG"
                                                 : "USB Serial";

        const std::string sys = "/sys/class/tty/" + node + "/device";
        std::error_code canonical_ec;
        const std::filesystem::path device =
            std::filesystem::canonical(sys, canonical_ec);
        const std::string base = canonical_ec ? sys : device.string();
        info.vendor_id = readSysfsValue(base + "/../idVendor");
        info.product_id = readSysfsValue(base + "/../idProduct");
        info.manufacturer = readSysfsValue(base + "/../manufacturer");
        info.product = readSysfsValue(base + "/../product");
        info.is_esp32 = isEspUsbVendor(info.vendor_id) ||
                        info.manufacturer.find("Espressif") != std::string::npos;
        ports.push_back(std::move(info));
    }

    std::sort(ports.begin(), ports.end(),
              [](const UsbPortInfo& lhs, const UsbPortInfo& rhs) {
                  return lhs.port < rhs.port;
              });
    return ports;
}

} // namespace bawal::backend
