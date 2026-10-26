#pragma once

/**
 * @file fwd.hpp
 * @brief Unified forward declarations for BawalVision core services and subsystems.
 *
 * Provides clean, centralized type declarations to prevent duplicate namespace
 * pollution and minimize header compilation coupling across the monorepo.
 */

namespace bawal {

namespace backend {
class BackendState;
class Database;
class CrowApp;
class MjpegIngress;
class VisionWorker;
struct ServerConfig;
}  // namespace backend

namespace lambda {
class LambdaRuntimeHandler;
struct LambdaConfig;
}  // namespace lambda

namespace server {
class RollingImageStore;
}  // namespace server

namespace vision {
class FoodClassifier;
class InspectionPipeline;
}  // namespace vision

namespace ipc {
class ShmProducer;
class ShmFrameConsumer;
}  // namespace ipc

}  // namespace bawal
