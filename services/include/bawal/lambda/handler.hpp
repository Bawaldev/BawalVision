#pragma once

#include <memory>
#include "bawal/lambda/types.hpp"
#include "bawal/lambda/events.hpp"
#include "bawal/vision/pipeline.hpp"

namespace bawal::lambda {

/**
 * @brief Master AWS Lambda runtime handler interface.
 * Coordinates cold-start pre-warming, single-pass pipeline execution, and event dispatch.
 */
class LambdaRuntimeHandler {
public:
    LambdaRuntimeHandler() = default;
    virtual ~LambdaRuntimeHandler() = default;

    /**
     * @brief Initialize AWS Lambda environment, thread pools, and master vision pipeline.
     */
    [[nodiscard]] virtual bool initialize(const LambdaConfig& config) = 0;

    /**
     * @brief Process a single tray inspection snapshot event (Dewarp -> Enhance -> INT8 YOLOv8 -> SIMD Spoilage).
     */
    [[nodiscard]] virtual LambdaInspectionResponse handleInspectionEvent(const LambdaInspectionRequest& request) = 0;

    /**
     * @brief Execute the official AWS Lambda Custom Runtime polling loop (/runtime/invocation/next).
     */
    virtual void runLoop() = 0;
};

} // namespace bawal::lambda
