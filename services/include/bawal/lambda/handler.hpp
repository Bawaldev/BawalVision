#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "bawal/lambda/batch.hpp"
#include "bawal/lambda/events.hpp"
#include "bawal/lambda/types.hpp"

namespace bawal::vision {
class InspectionPipeline;
}

namespace bawal::lambda {

/**
 * @brief Master AWS Lambda runtime handler.
 *
 * Coordinates cold-start pre-warming, single-pass pipeline execution, and
 * event dispatch. The handler owns its own InspectionPipeline so it can be
 * embedded in the Crow backend or a standalone Lambda bootstrap without
 * sharing mutable inference state with the live streaming worker.
 */
class LambdaRuntimeHandler {
public:
    LambdaRuntimeHandler() = default;
    virtual ~LambdaRuntimeHandler() = default;

    /**
     * @brief Initialize the AWS Lambda environment and the vision pipeline.
     */
    [[nodiscard]] virtual bool initialize(const LambdaConfig& config) = 0;

    /**
     * @brief Process a single tray inspection snapshot event.
     */
    [[nodiscard]] virtual LambdaInspectionResponse handleInspectionEvent(
        const LambdaInspectionRequest& request) = 0;

    /**
     * @brief Dispatch any supported payload: a single snapshot or a batch
     *        telemetry/log summary.
     */
    [[nodiscard]] virtual LambdaInspectionResponse handleEvent(
        std::string_view event_payload) = 0;

    /**
     * @brief Execute the AWS Lambda Custom Runtime polling loop.
     */
    virtual void runLoop() = 0;

    /**
     * @brief Whether the pipeline has been initialized and can accept events.
     */
    [[nodiscard]] virtual bool ready() const noexcept = 0;
};

/**
 * @brief Concrete ONNX/OpenCV implementation of LambdaRuntimeHandler.
 *
 * Initialization is lazy by default when embedded in the Crow backend: the
 * model is loaded on the first request instead of at process start. The
 * standalone Lambda bootstrap initializes eagerly so cold-start latency is
 * paid before the first invocation is accepted.
 */
class VisionLambdaHandler final : public LambdaRuntimeHandler {
public:
    VisionLambdaHandler();
    ~VisionLambdaHandler() override;

    VisionLambdaHandler(const VisionLambdaHandler&) = delete;
    VisionLambdaHandler& operator=(const VisionLambdaHandler&) = delete;

    [[nodiscard]] bool initialize(const LambdaConfig& config) override;
    [[nodiscard]] LambdaInspectionResponse handleInspectionEvent(
        const LambdaInspectionRequest& request) override;
    [[nodiscard]] LambdaInspectionResponse handleEvent(
        std::string_view event_payload) override;
    void runLoop() override;

    [[nodiscard]] bool ready() const noexcept override {
        return m_configured.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] bool initializeLocked(const LambdaConfig& config);
    [[nodiscard]] LambdaInspectionResponse handleLocked(
        const LambdaInspectionRequest& request);

    mutable std::mutex m_mutex;
    LambdaConfig m_config;
    std::unique_ptr<bawal::vision::InspectionPipeline> m_pipeline;
    BatchSummaryProcessor m_batch_processor;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_configured{false};
};

} // namespace bawal::lambda
