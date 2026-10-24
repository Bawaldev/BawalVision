#include "bawal/lambda/events.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "bawal/lambda/base64.hpp"
#include "bawal/server/inspection_dto.hpp"

namespace bawal::lambda {
namespace {

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

} // namespace

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

bool LambdaEventParser::parseEvent(std::string_view event_payload,
                                   LambdaInspectionRequest& out_request) {
    if (event_payload.empty()) {
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
        // Processing dimensions are optional tuning hints carried by the event.
        if (event.processing_width > 0) {
            out_request.processing_width = event.processing_width;
        }
        if (event.processing_height > 0) {
            out_request.processing_height = event.processing_height;
        }
    }

    if (out_request.raw_image_bytes.empty()) {
        return false;
    }
    out_request.image_content_type =
        std::string(detectContentType(out_request.raw_image_bytes));
    return true;
}

std::string LambdaResponseBuilder::buildResponse(const LambdaInspectionResponse& response,
                                                 InvocationSource source) {
    if (response.json_payload.empty()) {
        return "{}";
    }

    if (source == InvocationSource::DIRECT_INVOKE) {
        return response.json_payload;
    }

    bawal::server::LambdaProxyResponse proxy;
    proxy.status_code = response.status_code;
    proxy.is_base64_encoded = false;
    proxy.headers.content_type = "application/json";
    proxy.body = response.json_payload;

    std::string encoded;
    if (const auto error = glz::write_json(proxy, encoded); error) {
        return response.json_payload;
    }
    return encoded;
}

} // namespace bawal::lambda
