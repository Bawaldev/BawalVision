# Security Policy

## Supported Versions

The following versions of BawalVision receive security patches and vulnerability updates:

| Version | Supported          | Notes |
| ------- | ------------------ | ----- |
| `main`  | :white_check_mark: | Active development branch |
| `v1.x`  | :white_check_mark: | Production releases |
| `< 1.0` | :x:                | Deprecated / Pre-release |

---

## Reporting a Vulnerability

The BawalVision team takes security seriously, especially given our target deployment on edge hardware (ESP32-S3), industrial vision microservices, and cloud ingestion pipelines.

### ⚠️ Please Do NOT Report Security Vulnerabilities via Public GitHub Issues!

If you discover a security vulnerability, credential leak, or critical exploit:

1. **GitHub Private Vulnerability Reporting (Preferred)**:
   - Go to the **Security** tab of the repository: [BawalVision Security Advisories](https://github.com/fariswirakusuma/BawalVision/security/advisories).
   - Click **"Report a vulnerability"** to draft a private advisory.

2. **Direct Maintainer Contact**:
   - Send an encrypted or direct email to the project maintainers:
     - Faris Wirakusuma: `fariswkt@gmail.com`

### What to Include in Your Report:
- A description of the vulnerability and its potential impact.
- Clear reproduction steps or Proof of Concept (PoC).
- Affected subsystem (e.g. Go Fiber REST API, C++ OpenCV memory buffer, ESP32 Wi-Fi authentication).
- Suggested remediation or patch (if available).

### Response SLA:
- We acknowledge receipt within **48 hours**.
- We aim to release a coordinated patch or advisory within **7 to 14 business days**.

