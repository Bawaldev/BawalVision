# Gemini CLI Guidelines & Directives

## Model Specifications
- **Minimum Model Version:** Gemini 3.5 or higher (e.g., `gemini-3.5-flash`, `gemini-3.7-flash`, or `gemini-2.5-pro` with reasoning enabled).
- **Execution Mode:** Supports explicit Agentic, Copilot, and Chat modes.

## Interaction & Mode Switching Protocols
Switch modes explicitly by starting a prompt with the mode keyword:

1. **`[MODE: CHAT]` (Default / Discussion):**
   - Pure Q&A, brainstorming, or syntax checks.
   - Do NOT execute shell commands or modify files directly. Only provide recommendations or code snippets.

2. **`[MODE: AGENT]` (Autonomous Execution):**
   - Full agentic operation for multi-step tasks, refactoring, or running builds.
   - Authorized to inspect files, execute shell commands via `!`, create necessary temp files, and write code changes directly.
   - Must log every execution step and reasoning into `OUTPUT.md`.

3. **`[MODE: COPILOT]` (Interactive Review):**
   - Propose changes step-by-step. Ask for confirmation before modifying critical codebase files.

## Project Execution & Output Protocol
1. **Change Documentation:**
   - Always explain the reasoning behind any code modification, structural adjustment, or refactoring in `OUTPUT.md` before or immediately after applying changes.
   - Include the scope of changes, affected modules, and performance or functional benefits.

2. **Cache & Temporary Artifact Management:**
   - Route all build artifacts, temporary logs, and session caches to a designated temporary folder: `.cache_trash/`.
   - Never write transient build files or intermediate outputs to the root directory.