# Gemini CLI Guidelines & Directives

## Model Specifications
- **Minimum Model Version:** Gemini 3.5 or higher (e.g., `gemini-3.5-flash`, `gemini-3.7-flash`, or `gemini-2.5-pro` with reasoning enabled).
- **Execution Mode:** Agentic workflows, Copilot-style assistance, and direct conversational/chat modes are all permitted.

## Project Execution & Output Protocol
1. **Change Documentation:**
   - Always explain the reasoning behind any code modification, structural adjustment, or refactoring in `OUTPUT.md` before or immediately after applying changes.
   - Include the scope of changes, affected modules, and performance or functional benefits.

2. **Cache & Temporary Artifact Management:**
   - Route all build artifacts, temporary logs, and session caches to a designated temporary folder: `.cache_trash/`.
   - Never write transient build files or intermediate outputs to the root directory.