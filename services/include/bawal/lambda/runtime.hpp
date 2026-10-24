#pragma once

#include "bawal/lambda/handler.hpp"
#include "bawal/lambda/types.hpp"

namespace bawal::lambda {

/**
 * @brief Run the AWS Lambda custom runtime loop for the supplied handler.
 *
 * Returns a process exit code. When the project is built without
 * `ENABLE_AWS_LAMBDA`, the loop is unavailable and this returns 1.
 */
[[nodiscard]] int runLambdaRuntime(LambdaRuntimeHandler& handler,
                                   const LambdaConfig& config);

} // namespace bawal::lambda
