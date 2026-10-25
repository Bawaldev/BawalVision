#pragma once

#include <string>

namespace bawal::server {
struct InspectionResponse;
}

namespace bawal::vision::image {
struct BatchReportSummary;
}

namespace bawal::report {

[[nodiscard]] std::string buildBatchMarkdown(
    const vision::image::BatchReportSummary& summary);

[[nodiscard]] std::string buildLambdaInspectionMarkdown(
    const server::InspectionResponse& inspection);

} // namespace bawal::report
