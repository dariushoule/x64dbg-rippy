# x64dbg-rippy

AI assistant plugin for x64dbg with LLM tool-use for debugger interaction. Embeds a WebView2 chat pane and connects to Anthropic/OpenAI-compatible APIs.

## Build

Requires Visual Studio 2022 with C++ workload and vcpkg integration.

```bash
# Build + install 64-bit (preferred — builds frontend, compiles, copies to x64dbg plugins dir)
build-install-64.cmd

# Build + install 32-bit
build-install-32.cmd

# Manual cmake (if needed)
cmake -B build64 -G "Visual Studio 17 2022" -A x64
cmake --build build64 --config Release
```

Output: `build64/Release/x64dbg-rippy.dp64` or `build32/Release/x64dbg-rippy.dp32`

## Project Structure

- `cmake.toml` — cmkr project definition (generates CMakeLists.txt). Source of truth for build config.
- `cmake/` — CMake helpers (x64dbg SDK, static runtime, configs)
- `src/pluginmain.h/cpp` — DLL entry, plugin exports, SDK includes
- `src/plugin.h/cpp` — Menu registration, settings dialog, plugin init
- `src/config.h` — Shared constants (settings keys, provider names, defaults, API version)
- `src/chat_panel.h/cpp` — WebView2-in-QWidget tab, tool-use loop, message dispatch
- `src/api_client.h/cpp` — HTTP client for LLM APIs (Anthropic + OpenAI format translation)
- `src/tools.h/cpp` — Debugger tool definitions and executor (calls x64dbg SDK directly)
- `src/icon_data.h` — Embedded tab icon
- `src/resource.h`, `src/plugin.rc` — Win32 dialog resources, chat_ui.html embed
- `src/chat_ui.html` — **Generated** by frontend build. Do not edit directly.

### Frontend (`frontend/`)

- `src/index.jsx` — Preact chat UI components
- `src/styles.css` — Dark terminal theme
- `build.mjs` — esbuild bundles JSX+CSS, inlines into `index.html`, outputs `src/chat_ui.html`
- `index.html` — HTML template with `<!-- __CSS__ -->` / `<!-- __JS__ -->` placeholders
- Build: `cd frontend && npm run build`
- Note: `build.mjs` uses arrow-function replacement (`() => ...`) to avoid `$&` interpolation bugs in String.replace.

## Architecture

### Tool-use flow

The LLM has debugger tools (read_memory, disassemble, get_registers, etc.) that call the x64dbg SDK directly in-process. The tool loop:

1. Background thread sends HTTP request to LLM API with tool definitions
2. If response contains `tool_use` blocks, each tool call is marshaled to the GUI thread via `GuiExecuteOnGuiThreadEx` + Win32 Event for synchronization
3. Tool results are appended and the loop continues (up to 25 rounds)
4. Final text response is posted to the frontend

x64dbg SDK calls **must** run on the GUI thread. HTTP calls run on the background thread.

### Message history

Internal format uses Anthropic-style content blocks (`vector<json>`). The api_client translates to/from OpenAI wire format as needed.

## Settings

Stored in x64dbg.ini under `[Rippy]` section. Keys defined in `config.h`:
- `Provider` — "anthropic" or "openai"
- `ApiKey` — API key
- `Endpoint` — Base URL
- `Model` — Model name

## Command Reference

The `get_command_help` tool searches an embedded x64dbg command reference (`rc/x64dbg-command-ref.txt`). To regenerate it after updating x64dbg source:

```bash
python rc/generate-command-ref.py [path-to-x64dbg-source]
# Defaults to C:\code\x64dbg
```

This synthesizes `docs/introduction/` and `docs/commands/` from the x64dbg repo into a single text file embedded as `IDR_COMMAND_REF` in `plugin.rc`.

## Dependencies (vcpkg)

- `qt5-base` — QWidget for x64dbg tab integration
- `cpp-httplib[openssl]` — HTTPS client
- `nlohmann-json` — JSON serialization
- `webview2` — Embedded browser control

## Frontend Dependencies (npm)

- `preact` — Lightweight UI framework
- `marked` — Markdown rendering
- `dompurify` — HTML sanitization
- `esbuild` — Bundler (dev)
