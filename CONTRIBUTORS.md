# Project Contributors & Core Team 👥🏆

> **BawalVision — Smart Food Quality & Tray Inspection System**  
> *Developed for the OpenCV AI Competition 2026*

BawalVision is conceptualized, designed, and actively engineered by the following core team members:

---

## 🌟 Core Maintainers & Subsystem Leads

| Contributor | GitHub Handle | Primary Subsystems & Architectural Ownership |
| :--- | :--- | :--- |
| **Faris Wirakusuma** | [@fariswirakusuma](https://github.com/fariswirakusuma) | **Lead System Architect & Backend Lead**<br/>• C++20 Vision Engine (`libbawal_core`) & AVX2 SIMD vectorization.<br/>• Zero-copy POSIX Shared Memory (`/dev/shm`) frame ingress.<br/>• Crow C++20 REST/WebSocket server, POSIX SHM worker, and PostgreSQL persistence.<br/>• AWS Lambda C++ Custom Runtime packaging & security hardening.<br/>• ThreadSanitizer audit and DevOps CI/CD pipeline automation. |
| **Rasya Khairi** | [@OurangIlang](https://github.com/OurangIlang) | **Machine Learning & Frontend Lead**<br/>• Autonomous dataset gathering, MD5 & perceptual dHash deduplication.<br/>• Hard-negative tableware collection & empty plate sample injection.<br/>• YOLO-World zero-shot bounding box relabeling pipeline.<br/>• YOLOv8 cold training, layer-frozen fine-tuning (transfer learning), and INT8 Post-Training Quantization (PTQ).<br/>• SvelteKit 5 web HMI dashboard and DaisyUI visual components. |
| **Daffa Rizqy Andika** | [@pondwatr](https://github.com/pondwatr) | **Embedded Systems & Hardware Lead**<br/>• ESP32-S3 FreeRTOS firmware with 100% static memory allocation.<br/>• OV5640 DVP camera driver and DMA frame queueing.<br/>• ST7789 SPI LCD driver with LVGL 8 RGB operator dashboard.<br/>• Contactless infrared temperature sensing (MLX90614/GY-906) & hardware wiring schematics. |

---

## 🏛️ Codeowners & Ownership Mapping

Subsystem review responsibilities are defined in [`.github/CODEOWNERS`](.github/CODEOWNERS):

* **Embedded Hardware (`edge/`)**: `@pondwatr`, `@fariswirakusuma`
* **C++ Vision & Backend (`services/`)**: `@fariswirakusuma`
* **Machine Learning & Datasets (`ml/`)**: `@OurangIlang`, `@fariswirakusuma`
* **Frontend Web Dashboard (`frontend/`)**: `@OurangIlang`, `@fariswirakusuma`
* **DevOps, Docker & CI/CD (`deploy/`, `.github/`)**: `@fariswirakusuma`

---

## 🤝 Open Source Foundations & Acknowledgments

BawalVision is built on top of high-performance open-source projects:

* **[OpenCV](https://opencv.org/)**: Computer vision primitives, image filtering, camera calibration, and perspective transformation.
* **[ONNX Runtime](https://onnxruntime.ai/)**: High-performance cross-platform machine learning inference with CPU AVX2 acceleration.
* **[Ultralytics YOLOv8](https://github.com/ultralytics/ultralytics)**: Real-time object detection architecture.
* **[Crow C++](https://crowcpp.org/)**: Fast, header-only C++ web micro-framework.
* **[FreeRTOS](https://www.freertos.org/) & [ESP-IDF](https://github.com/espressif/esp-idf)**: Real-time operating system and development framework for ESP32-S3.
* **[LVGL](https://lvgl.io/)**: Embedded GUI graphics library for microcontrollers.
* **[mimalloc](https://github.com/microsoft/mimalloc)**: Microsoft's compact general-purpose allocator with high concurrency scalability.
* **[Glaze](https://github.com/stephenberry/glaze)**: Extremely fast C++ JSON serialization with compile-time reflection.
* **[SvelteKit](https://kit.svelte.dev/)**: Next-generation web framework for reactive user interfaces.

---

## 🚀 How to Contribute

Interested in contributing to BawalVision?
1. Review our [Contributing Guidelines](.github/CONTRIBUTING.md) to understand our coding standards, branch conventions, and local verification checks.
2. Read the [Code of Conduct](.github/CODE_OF_CONDUCT.md) to maintain an open and welcoming community.
3. Check open issues labeled `good first issue` or `help wanted` on our GitHub repository.

