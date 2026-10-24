# Security Policy 🛡️

> **Security & Vulnerability Disclosure Policy for BawalVision**  
> *Industrial Edge-to-Cloud Inspection Architecture (OpenCV AI Competition 2026)*

BawalVision takes the security of its edge devices, computer vision pipelines, web interfaces, and cloud functions seriously. This document outlines our vulnerability disclosure policy, supported versions, security architecture guidelines, and reporting procedures.

---

## 📌 Supported Versions

We provide security patches, container rebuilds, and vulnerability fixes for the following versions:

| Version | Status | Supported | Notes |
| :--- | :---: | :---: | :--- |
| **`main`** | Active Development | :white_check_mark: | Continuous integration, latest bug fixes and security patches. |
| **`v1.x`** | Stable Release | :white_check_mark: | Production container images and edge firmware builds. |
| **`< 1.0`** | Deprecated | :x: | Legacy pre-release prototypes; unsupported. |

---

## 🔒 Security Architecture & Guarantees

BawalVision's security model covers three distinct architectural tiers:

### 1. Edge IoT Node (`edge/`)
* **Deterministic Memory Model**: Employs static FreeRTOS task stacks (`xTaskCreateStaticPinnedToCore`) and queues (`xQueueCreateStatic`) to eliminate heap fragmentation and buffer overflow attack vectors.
* **Network Isolation**: Operates over internal LAN Wi-Fi with frame transmission constrained to dedicated HTTP Keep-Alive sockets and local UDP discovery (port `8765`).
* **Non-Volatile Storage (NVS)**: Wi-Fi credentials are baked into compile-time definitions or written into flash partitions with wear-leveling and validation.

### 2. C++20 Vision Backend & Ingress (`services/`)
* **Thread & Memory Safety**: Routinely audited using LLVM **ThreadSanitizer (TSan)** and AddressSanitizer (ASan) to prevent data races, lock inversion, and memory corruption.
* **Zero-Copy IPC Isolation**: Inter-process frame ingress utilizes POSIX Shared Memory (`/dev/shm`) protected by monotonic sequence locks with strict memory barriers.
* **Architecture Portability**: Disallows unsafe `-march=native` compiler flags in production builds; AVX2 vectorization includes dynamic bounds checking and portable OpenMP fallback loops.

### 3. Container & Cloud Deployment (`deploy/`)
* **Minimal Base Images**: Multi-stage Docker builds based on `ubuntu:24.04` and `node:22-alpine` stripping development toolchains (`build-essential`, `g++`, headers) from final runtime layers.
* **Package Hygiene**: Lambda and backend containers execute automated `apt-get upgrade -y` and purge unused video capture/encoding packages (`ffmpeg`, `libavdevice*`), ensuring clean AWS ECR Inspector scans with zero Critical or High CVEs.
* **Database Access**: PostgreSQL credentials and connections are passed via environment variables (`POSTGRES_PASSWORD`, `DB_HOST`), with SSL support configurable via `DB_SSLMODE`.

---

## 🚨 Reporting a Vulnerability

If you discover a security vulnerability, credential exposure, or memory safety defect in BawalVision, **please do NOT disclose it through public GitHub issues or discussions.**

### Preferred Reporting Channels:

1. **GitHub Private Vulnerability Advisory (Recommended)**:
   - Navigate to the **Security** tab of the repository: [BawalVision Security Advisories](https://github.com/Bawaldev/BawalVision/security/advisories).
   - Click **"Report a vulnerability"** to open a confidential report directly with the core maintainers.

2. **Direct Maintainer Contact**:
   - Send an encrypted or direct email to the core architecture maintainer:
     - **Faris Wirakusuma**: `fariswkt@gmail.com`

### What to Include in Your Report:
* **Summary**: A clear description of the vulnerability and its potential impact.
* **Reproduction Steps**: Minimal steps or Proof-of-Concept (PoC) script demonstrating the issue.
* **Affected Subsystem**: Specify the impacted module (e.g., Crow C++ backend, OpenCV SHM ingress, ESP32 FreeRTOS Wi-Fi stack, SvelteKit HMI, or AWS Lambda runtime).
* **Suggested Fix**: Proposed patch, sanitization check, or configuration change (if available).

### Response Service Level Agreement (SLA):
* **Initial Acknowledgment**: Within **48 hours** of report receipt.
* **Triage & Assessment**: Within **5 business days** with severity rating (CVSS score).
* **Coordinated Patch & Release**: Within **14 business days** along with public credit in release notes (unless anonymity is requested).

---

## 🚫 Prohibited Activities & Competition Disclosure

In compliance with the **OpenCV AI Competition 2026** rules and ethical research guidelines:
* Do not conduct automated volumetric stress testing, DoS, or credential brute-forcing against live production deployments.
* Never commit real Wi-Fi credentials, private API keys, dataset assets containing Personally Identifiable Information (PII), or unauthorized food vendor data to the Git history.
* All AI assistance or agentic program modifications are documented in [`OUTPUT.md`](OUTPUT.md) for competition audit transparency.
