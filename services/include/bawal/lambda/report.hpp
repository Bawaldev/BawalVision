#pragma once

#include <string>

#include <glaze/glaze.hpp>

namespace bawal::lambda {

struct LambdaReportRequest {
    std::string markdown;
    std::string title{"BawalVision Inspection Report"};
    std::string filename{"BawalVision-Report.pdf"};
};

} // namespace bawal::lambda

template <>
struct glz::meta<bawal::lambda::LambdaReportRequest> {
    using T = bawal::lambda::LambdaReportRequest;
    static constexpr auto value = glz::object(
        "markdown", &T::markdown,
        "title", &T::title,
        "filename", &T::filename);
};
