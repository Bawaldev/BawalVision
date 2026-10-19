#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include "bawal/lambda/types.hpp"

namespace bawal::lambda {

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

} // namespace bawal::lambda

