#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bawal/lambda/types.hpp"

namespace bawal::lambda {

[[nodiscard]] bool validateImagePayload(const std::vector<uint8_t>& image);

/**
 * @brief Abstract parser interface for decoding AWS Lambda invocation events.
 */
class IEventParser {
public:
    virtual ~IEventParser() = default;

    /**
     * @brief Parses an invocation event string into a structured inspection request.
     */
    [[nodiscard]] virtual bool parseEvent(std::string_view event_payload,
                                          LambdaInspectionRequest& out_request) = 0;
};

/**
 * @brief Abstract response builder interface for serializing serverless inspection outputs.
 */
class IResponseBuilder {
public:
    virtual ~IResponseBuilder() = default;

    /**
     * @brief Encodes inspection results into an AWS API Gateway or direct response envelope.
     */
    [[nodiscard]] virtual std::string buildResponse(const LambdaInspectionResponse& response,
                                                    InvocationSource source) = 0;
};

/**
 * @brief Default invocation parser.
 *
 * Accepts direct JSON invocations, API Gateway/Function URL payloads (v1/v2),
 * and a plain base64 image body. Image bytes may be supplied through
 * `image_base64`, `body`, or a `data:` URI.
 */
class LambdaEventParser final : public IEventParser {
public:
    [[nodiscard]] bool parseEvent(std::string_view event_payload,
                                  LambdaInspectionRequest& out_request) override;

    /** Classify an event payload without fully decoding the image. */
    [[nodiscard]] static InvocationSource detectSource(std::string_view event_payload);

    /** Classify and extract HTTP method/path from an invocation event without full decode. */
    [[nodiscard]] static LambdaHttpRoute extractHttpRoute(std::string_view payload);

    /** Read a case-insensitive HTTP header from an API Gateway/Function URL event. */
    [[nodiscard]] static std::string extractHeader(std::string_view payload,
                                                   std::string_view name);

    /** True when API Gateway already authenticated the request with AWS_IAM. */
    [[nodiscard]] static bool isIamAuthorized(std::string_view payload);

    /** Extract raw or base64-decoded body bytes from an invocation event. */
    [[nodiscard]] static std::vector<uint8_t> extractBodyBytes(std::string_view payload);
};

/**
 * @brief Default response builder for direct and proxy-style invocations.
 */
class LambdaResponseBuilder final : public IResponseBuilder {
public:
    [[nodiscard]] std::string buildResponse(const LambdaInspectionResponse& response,
                                            InvocationSource source) override;
};

} // namespace bawal::lambda
