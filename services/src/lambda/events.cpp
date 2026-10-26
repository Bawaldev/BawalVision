#include "bawal/lambda/events.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "bawal/lambda/base64.hpp"
#include "bawal/server/inspection_dto.hpp"

namespace {

using bawal::lambda::decodeBase64;
using bawal::lambda::kMaxImageBytes;
using bawal::lambda::kMaxImageDimension;
using bawal::lambda::kMaxImagePixels;

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool looksLikeBase64(std::string_view value) {
    if (value.size() < 64 || value.size() % 4 != 0) {
        return false;
    }
    for (const char ch : value) {
        if (ch == '=' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (kBase64Alphabet.find(ch) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

std::string_view stripDataUri(std::string_view value) {
    if (value.rfind("data:", 0) != 0) {
        return value;
    }
    const std::size_t comma = value.find(',');
    if (comma == std::string_view::npos) {
        return {};
    }
    return value.substr(comma + 1);
}

bool hasImageMagic(std::string_view bytes) {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xD8) {
        return true;
    }
    return bytes.size() >= 8 && raw[0] == 0x89 && raw[1] == 0x50 &&
           raw[2] == 0x4E && raw[3] == 0x47;
}

std::string_view detectContentType(const std::vector<uint8_t>& image) {
    if (image.size() >= 8 && image[0] == 0x89 && image[1] == 0x50 &&
        image[2] == 0x4E && image[3] == 0x47) {
        return "image/png";
    }
    if (image.size() >= 2 && image[0] == 0xFF && image[1] == 0xD8) {
        return "image/jpeg";
    }
    return "application/octet-stream";
}

uint32_t readBigEndian32(const unsigned char* data) {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

bool jpegDimensions(std::string_view bytes, int& width, int& height) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    std::size_t offset = 2;
    while (offset + 3 < bytes.size()) {
        if (data[offset] != 0xFF) {
            ++offset;
            continue;
        }
        const unsigned char marker = data[offset + 1];
        offset += 2;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (offset + 1 >= bytes.size()) {
            return false;
        }
        const std::size_t length =
            (static_cast<std::size_t>(data[offset]) << 8U) |
            static_cast<std::size_t>(data[offset + 1]);
        if (length < 2 || offset + length > bytes.size()) {
            return false;
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
            marker != 0xC8 && marker != 0xCC) {
            if (length < 7 || offset + 7 > bytes.size()) {
                return false;
            }
            height = (static_cast<int>(data[offset + 3]) << 8) |
                     static_cast<int>(data[offset + 4]);
            width = (static_cast<int>(data[offset + 5]) << 8) |
                    static_cast<int>(data[offset + 6]);
            return width > 0 && height > 0;
        }
        offset += length;
    }
    return false;
}

bool pngDimensions(std::string_view bytes, int& width, int& height) {
    static constexpr unsigned char signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 24 ||
        std::memcmp(bytes.data(), signature, sizeof(signature)) != 0 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
        return false;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const uint32_t parsed_width = readBigEndian32(data + 16);
    const uint32_t parsed_height = readBigEndian32(data + 20);
    if (parsed_width == 0 || parsed_height == 0 ||
        parsed_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        parsed_height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    width = static_cast<int>(parsed_width);
    height = static_cast<int>(parsed_height);
    return true;
}

bool imageWithinLimits(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > kMaxImageBytes) {
        return false;
    }
    int width = 0;
    int height = 0;
    const std::string_view view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!jpegDimensions(view, width, height) &&
        !pngDimensions(view, width, height)) {
        return false;
    }
    if (width <= 0 || height <= 0 || width > kMaxImageDimension ||
        height > kMaxImageDimension) {
        return false;
    }
    return static_cast<std::uint64_t>(width) *
               static_cast<std::uint64_t>(height) <=
           kMaxImagePixels;
}

std::optional<bawal::server::LambdaEvent> unwrapGatewayEvent(
    const bawal::server::LambdaEvent& outer) {
    if (outer.body.empty()) {
        return std::nullopt;
    }

    std::string decoded;
    std::string_view body = outer.body;
    if (outer.is_base64_encoded) {
        const std::vector<uint8_t> bytes = decodeBase64(stripDataUri(body));
        decoded.assign(bytes.begin(), bytes.end());
        body = decoded;
    }
    if (body.empty() || body.front() != '{') {
        return std::nullopt;
    }

    bawal::server::LambdaEvent inner;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(inner, body)) {
        return std::nullopt;
    }
    if (inner.image_base64.empty() && inner.body.empty() &&
        inner.device_id.empty() && inner.frame_id == 0) {
        return std::nullopt;
    }
    return inner;
}

std::vector<uint8_t> extractImageBytes(const bawal::server::LambdaEvent& event) {
    if (!event.image_base64.empty()) {
        return decodeBase64(stripDataUri(event.image_base64));
    }
    if (event.body.empty()) {
        return {};
    }

    std::string_view body = stripDataUri(event.body);
    if (event.is_base64_encoded) {
        return decodeBase64(body);
    }

    // API Gateway forwards binary payloads as base64 only when configured to
    // do so. Accept a base64 body even without the flag when the decoded
    // bytes carry a recognizable image signature.
    if (looksLikeBase64(body)) {
        std::vector<uint8_t> decoded = decodeBase64(body);
        if (!decoded.empty() && hasImageMagic(
                                    std::string_view(reinterpret_cast<const char*>(decoded.data()),
                                                     decoded.size()))) {
            return decoded;
        }
    }

    return std::vector<uint8_t>(body.begin(), body.end());
}

const glz::json_t* findJsonMember(const glz::json_t& object,
                                  std::string_view key) {
    const auto* map = object.get_if<glz::json_t::object_t>();
    if (map == nullptr) {
        return nullptr;
    }
    const auto it = map->find(key);
    if (it == map->end()) {
        return nullptr;
    }
    return &it->second;
}

bool hasArn(const glz::json_t* value) {
    if (value == nullptr) {
        return false;
    }
    const auto* text = value->get_if<std::string>();
    return text != nullptr && text->rfind("arn:aws:", 0) == 0;
}

} // namespace

namespace bawal::lambda {

bool validateImagePayload(const std::vector<uint8_t>& image) {
    return imageWithinLimits(image);
}

InvocationSource LambdaEventParser::detectSource(std::string_view payload) {
    if (payload.find("lambda-url") != std::string_view::npos ||
        payload.find("Lambda-URL") != std::string_view::npos) {
        return InvocationSource::FUNCTION_URL;
    }
    if (payload.find("\"requestContext\"") != std::string_view::npos ||
        payload.find("\"rawPath\"") != std::string_view::npos ||
        payload.find("\"isBase64Encoded\"") != std::string_view::npos) {
        return InvocationSource::API_GATEWAY_HTTP_V2;
    }
    return InvocationSource::DIRECT_INVOKE;
}

LambdaHttpRoute LambdaEventParser::extractHttpRoute(std::string_view payload) {
    LambdaHttpRoute route;
    if (payload.find("\"rawPath\"") != std::string_view::npos ||
        payload.find("\"requestContext\"") != std::string_view::npos ||
        payload.find("\"httpMethod\"") != std::string_view::npos) {
        route.is_http_event = true;
    } else {
        return route;
    }

    auto findField = [](std::string_view text, std::string_view key) -> std::string {
        size_t pos = text.find(key);
        if (pos == std::string_view::npos) return "";
        pos = text.find(':', pos + key.size());
        if (pos == std::string_view::npos) return "";
        pos = text.find('"', pos);
        if (pos == std::string_view::npos) return "";
        size_t end = text.find('"', pos + 1);
        if (end == std::string_view::npos) return "";
        return std::string(text.substr(pos + 1, end - (pos + 1)));
    };

    route.path = findField(payload, "\"rawPath\"");
    if (route.path.empty()) {
        route.path = findField(payload, "\"path\"");
    }

    std::string m = findField(payload, "\"method\"");
    if (m.empty()) {
        m = findField(payload, "\"httpMethod\"");
    }
    if (!m.empty()) {
        route.method = std::move(m);
    }
    return route;
}

bool LambdaEventParser::isIamAuthorized(std::string_view payload) {
    glz::json_t root;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(root, payload)) {
        return false;
    }
    const glz::json_t* request_context = findJsonMember(root, "requestContext");
    if (request_context == nullptr) {
        return false;
    }
    if (const glz::json_t* identity =
            findJsonMember(*request_context, "identity");
        identity != nullptr) {
        if (hasArn(findJsonMember(*identity, "userArn")) ||
            hasArn(findJsonMember(*identity, "caller"))) {
            return true;
        }
    }
    if (const glz::json_t* authorizer =
            findJsonMember(*request_context, "authorizer");
        authorizer != nullptr) {
        if (const glz::json_t* iam = findJsonMember(*authorizer, "iam");
            iam != nullptr) {
            if (hasArn(findJsonMember(*iam, "userArn")) ||
                hasArn(findJsonMember(*iam, "caller"))) {
                return true;
            }
        }
    }
    return false;
}

std::vector<uint8_t> LambdaEventParser::extractBodyBytes(std::string_view payload) {
    if (payload.empty()) {
        return {};
    }

    // Direct raw ZIP check (direct invocation with raw bytes)
    if (payload.size() >= 4 &&
        static_cast<uint8_t>(payload[0]) == 0x50 &&
        static_cast<uint8_t>(payload[1]) == 0x4B &&
        (static_cast<uint8_t>(payload[2]) == 0x03 || static_cast<uint8_t>(payload[2]) == 0x05) &&
        (static_cast<uint8_t>(payload[3]) == 0x04 || static_cast<uint8_t>(payload[3]) == 0x06)) {
        return std::vector<uint8_t>(payload.begin(), payload.end());
    }

    // Try parsing API Gateway / Function URL event envelope
    bawal::server::LambdaEvent event;
    if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(event, payload)) {
        if (!event.body.empty()) {
            std::string_view body = stripDataUri(event.body);
            if (event.is_base64_encoded || looksLikeBase64(body)) {
                std::vector<uint8_t> decoded = decodeBase64(body);
                if (!decoded.empty()) {
                    return decoded;
                }
            }
            return std::vector<uint8_t>(body.begin(), body.end());
        }
    }

    // Fallback: check if entire payload is base64 encoded
    if (looksLikeBase64(payload)) {
        std::vector<uint8_t> decoded = decodeBase64(stripDataUri(payload));
        if (!decoded.empty()) {
            return decoded;
        }
    }

    return std::vector<uint8_t>(payload.begin(), payload.end());
}

std::string LambdaEventParser::extractHeader(std::string_view payload,
                                             std::string_view name) {
    glz::json_t root;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(root, payload)) {
        return {};
    }
    const auto* object = root.get_if<glz::json_t::object_t>();
    if (object == nullptr) {
        return {};
    }
    const auto headers_it = object->find("headers");
    if (headers_it == object->end()) {
        return {};
    }
    const auto* headers = headers_it->second.get_if<glz::json_t::object_t>();
    if (headers == nullptr) {
        return {};
    }
    for (const auto& [key, value] : *headers) {
        if (key.size() != name.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(key[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        if (const auto* text = value.get_if<std::string>()) {
            return *text;
        }
    }
    return {};
}

bool LambdaEventParser::parseEvent(std::string_view event_payload,
                                   LambdaInspectionRequest& out_request) {
    if (event_payload.empty() || event_payload.size() > kMaxEventPayloadBytes) {
        return false;
    }

    out_request = LambdaInspectionRequest{};
    out_request.source = detectSource(event_payload);

    bawal::server::LambdaEvent event;
    const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(
        event, event_payload);

    if (error) {
        // A bare base64 image is also a valid direct invocation payload.
        const std::string_view stripped = stripDataUri(event_payload);
        if (!looksLikeBase64(stripped)) {
            return false;
        }
        out_request.raw_image_bytes = decodeBase64(stripped);
        out_request.source = InvocationSource::DIRECT_INVOKE;
    } else {
        // API Gateway proxy integrations wrap the real JSON payload in `body`.
        // Unwrap it so gateway callers can send the same snapshot schema as
        // direct Lambda invocations.
        if (out_request.source != InvocationSource::DIRECT_INVOKE) {
            if (auto inner = unwrapGatewayEvent(event); inner.has_value()) {
                event = std::move(*inner);
            }
        }
        out_request.raw_image_bytes = extractImageBytes(event);
        out_request.device_id = event.device_id.empty() ? "ESP32S3_INSPECT"
                                                        : event.device_id;
        out_request.frame_id = event.frame_id;
        out_request.confidence_threshold = static_cast<float>(event.confidence_threshold);
        out_request.return_annotated = event.return_annotated;
        if (!out_request.return_annotated) {
            const std::string ret_hdr = extractHeader(event_payload, "x-return-annotated");
            if (ret_hdr == "1" || ret_hdr == "true" || ret_hdr == "yes") {
                out_request.return_annotated = true;
            }
        }
        // Processing dimensions are optional tuning hints carried by the event.
        if (event.processing_width > 0) {
            out_request.processing_width = event.processing_width;
        }
        if (event.processing_height > 0) {
            out_request.processing_height = event.processing_height;
        }
    }

    if (out_request.raw_image_bytes.empty() ||
        out_request.raw_image_bytes.size() > kMaxImageBytes ||
        !imageWithinLimits(out_request.raw_image_bytes)) {
        return false;
    }
    out_request.image_content_type =
        std::string(detectContentType(out_request.raw_image_bytes));
    return true;
}

std::string LambdaResponseBuilder::buildResponse(const LambdaInspectionResponse& response,
                                                 InvocationSource source) {
    if (response.is_binary && !response.binary_body.empty()) {
        const std::string encoded = encodeBase64(response.binary_body);
        if (source == InvocationSource::DIRECT_INVOKE) {
            return std::string(R"({"status":"success","content_type":")") +
                   response.content_type + R"(","is_base64_encoded":true,"body":")" +
                   encoded + R"("})";
        }

        bawal::server::LambdaProxyResponse proxy;
        proxy.status_code = response.status_code;
        proxy.is_base64_encoded = true;
        proxy.headers.content_type = response.content_type;
        proxy.headers.content_disposition = response.content_disposition;
        proxy.headers.x_report_format = "pdf";
        proxy.body = encoded;
        std::string json;
        if (const auto error = glz::write_json(proxy, json); error) {
            return "{\"error\":\"binary response serialization failed\"}";
        }
        return json;
    }

    if (response.json_payload.empty()) {
        return "{}";
    }

    if (source == InvocationSource::DIRECT_INVOKE) {
        return response.json_payload;
    }

    bawal::server::LambdaProxyResponse proxy;
    proxy.status_code = response.status_code;
    proxy.is_base64_encoded = false;
    proxy.headers.content_type = response.content_type;
    proxy.body = response.json_payload;

    std::string encoded;
    if (const auto error = glz::write_json(proxy, encoded); error) {
        return response.json_payload;
    }
    return encoded;
}

} // namespace bawal::lambda
