# Mock Testing Video Assets (`examples/`)

This directory is designated for storing sample MP4 inspection videos used for offline benchmarking, automated testing, and CI mock runs without requiring a physical ESP32-S3 camera.

## Usage in C++ Vision Engine

Place your test video files in category subdirectories (e.g. `examples/banana/banana_sample_one.mp4`).

In `services/src/main.cpp` or your inspection scripts:

```cpp
#include "bawal/vision/video_source.hpp"

bawal::vision::VideoSource source;
if (source.open(bawal::vision::SourceMode::LOCAL_MP4, "examples/banana/banana_sample_one.mp4")) {
    cv::Mat frame;
    while (source.grabNextFrame(frame)) {
        // Run AI inspection pipeline on looped frame...
    }
}
```

The `LOCAL_MP4` mode will automatically rewind to the beginning upon reaching EOF, providing an infinite loop for continuous inspection testing. Supported formats include `.mp4` and `.webm`. Both the Go backend API (`/api/v1/examples`) and the frontend mocker dynamically scan these subdirectories.

## Note on Video File Tracking

Large binary MP4 files placed here are excluded from git commits by `.gitignore`. Keep this directory lightweight by relying on external artifact storage or downloading mock clips on-demand.

