#include "bawal/lambda/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "bawal/lambda/events.hpp"

#if defined(BAWAL_HAS_AWS_LAMBDA)
#include <aws/lambda-runtime/runtime.h>
#endif

namespace bawal::lambda {

int runLambdaRuntime(LambdaRuntimeHandler& handler, const LambdaConfig& config) {
#if defined(BAWAL_HAS_AWS_LAMBDA)
    if (!config.runtime_api_endpoint.empty()) {
        setenv("AWS_LAMBDA_RUNTIME_API", config.runtime_api_endpoint.c_str(), 1);
    }

    auto runtime_handler = [&handler](const aws::lambda_runtime::invocation_request& request) {
        try {
            const LambdaInspectionResponse result =
                handler.handleEvent(request.payload);
            const InvocationSource source =
                LambdaEventParser::detectSource(request.payload);
            LambdaResponseBuilder builder;
            return aws::lambda_runtime::invocation_response::success(
                builder.buildResponse(result, source), "application/json");
        } catch (const std::exception& error) {
            return aws::lambda_runtime::invocation_response::failure(
                error.what(), "ExecutionError");
        } catch (...) {
            return aws::lambda_runtime::invocation_response::failure(
                "Unknown execution error", "ExecutionError");
        }
    };

    aws::lambda_runtime::run_handler(runtime_handler);
    return 0;
#else
    (void)handler;
    (void)config;
    std::fprintf(stderr,
                 "[FATAL] This binary was built without ENABLE_AWS_LAMBDA; "
                 "the Lambda runtime loop is unavailable.\n");
    return 1;
#endif
}

} // namespace bawal::lambda
