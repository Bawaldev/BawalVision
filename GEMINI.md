# Gemini CLI Guidelines & Directives

## Model Specifications & Cost Management
- **Minimum Model Version:** Gemini 3.5 or higher (e.g., `gemini-3.5-flash`, `gemini-3.7-flash`, or `gemini-2.5-pro` with reasoning enabled).
- **Default Low-Cost Model:** Use `gemini-3.5-flash` or `gemini-3.7-flash` for daily chat and routine operations to stay within free-tier limits.
- **Execution Mode:** Supports explicit Agentic, Copilot, and Chat modes.

## Interaction & Mode Switching Protocols
Start prompts with the mode keyword to set execution boundaries and model behavior:

1. **`[MODE: FREE_CHAT]` (Minimal Quota / Read-Only):**
   - Pure Q&A, standard explanations, and light syntax checks using lightweight models (`gemini-3.5-flash`).
   - Strictly read-only context. NO shell execution (`!`) and NO file modifications.

2. **`[MODE: CHAT]` (Standard Discussion):**
   - Deep architectural analysis, complex math/algorithmic discussions (e.g., SVD, PCA, OpenCV APIs), or code review.
   - Read-only access. Direct file edits and shell executions are prohibited.

3. **`[MODE: COPILOT]` (Interactive Review):**
   - Interactive code generation and refactoring.
   - Proposes changes step-by-step and requests user confirmation before writing to files.

4. **`[MODE: AGENT]` (Autonomous Execution):**
   - Full autonomous operation for multi-step tasks, build testing, and automated fixes.
   - Authorized to inspect files, run shell commands via `!`, create build artifacts in `.cache_trash/`, and modify source code directly.
   - Must log every execution step and reasoning into `OUTPUT.md`.

## Competition Compliance & Ethical Safeguards (OpenCV AI Competition 2026)
1. **Agentic Disclosure & Transparency:**
   - All AI-assisted or agentic modifications must be explicitly documented in `OUTPUT.md` to guarantee competition compliance ("Disclosed agents and automated programs").
   - Never misrepresent benchmark results, unit test outcomes, or automated review outputs.
2. **IP & Sensitive Data Protection:**
   - Do NOT log, output, or store private API keys, deployment credentials, or confidential dataset assets in public files, reports, or commit logs.
3. **Responsible Operation:**
   - Do NOT generate code, scripts, or pipelines that create material safety, privacy, surveillance, or security risks.

## Project Execution & Output Protocol
1. **Change Documentation:**
   - Always explain the reasoning behind any code modification, structural adjustment, or refactoring in `OUTPUT.md` before or immediately after applying changes.
   - Include the scope of changes, affected modules, and performance or functional benefits.

2. **Cache & Temporary Artifact Management:**
   - Route all build artifacts, temporary logs, and session caches to `.cache_trash/`.
   - Never write transient build files or intermediate outputs to the root directory.

## Token Usage & Estimation Protocol
At the end of responses in `[MODE: AGENT]` or `[MODE: COPILOT]`, append an estimated token breakdown to `OUTPUT.md`:

```text
---
[Estimated Token Usage]
- Context Input: ~<est_input_tokens> tokens
- Generated Output: ~<est_output_tokens> tokens
- Cumulative Estimate: ~<total_tokens_used_this_session> / <model_context_limit> tokens
---