# Security Policy 🛡️

> **Security & Vulnerability Disclosure Policy for BawalVision**  
> *Industrial Edge-to-Cloud Inspection Architecture (OpenCV AI Competition 2026)*

BawalVision provides security patches, container rebuilds, and vulnerability fixes for the following versions:

| Version | Status | Supported | Notes |
| :--- | :---: | :---: | :--- |
| **`main`** | Active Development | :white_check_mark: | Continuous integration, latest bug fixes and security patches. |
| **`v1.x`** | Stable Release | :white_check_mark: | Production container images and edge firmware builds. |
| **`< 1.0`** | Deprecated | :x: | Legacy pre-release prototypes; unsupported. |

---

## 🚨 Reporting a Vulnerability

The BawalVision team takes security seriously across embedded hardware (ESP32-S3), industrial C++20 vision microservices, web dashboards, and AWS Lambda serverless functions.

### ⚠️ Please Do NOT Report Security Vulnerabilities via Public Issues!

If you discover a security vulnerability, credential leak, or critical exploit:

1. **GitHub Private Vulnerability Reporting (Preferred)**:
   - Go to the **Security** tab of the repository: [BawalVision Security Advisories](https://github.com/Bawaldev/BawalVision/security/advisories).
   - Click **"Report a vulnerability"** to draft a private advisory.

2. **Direct Maintainer Contact**:
   - Send an encrypted or direct email to the project maintainers:
     - **Faris Wirakusuma**: `fariswkt@gmail.com`

### What to Include in Your Report:
- A description of the vulnerability and its potential impact.
- Clear reproduction steps or Proof of Concept (PoC).
- Affected subsystem (e.g. C++ Crow REST API, POSIX SHM ring buffer, OpenCV SIMD evaluator, ESP32 Wi-Fi authentication, or SvelteKit frontend).
- Suggested remediation or patch (if available).

### Response SLA:
- We acknowledge receipt within **48 hours**.
- We aim to release a coordinated patch or advisory within **7 to 14 business days**.
