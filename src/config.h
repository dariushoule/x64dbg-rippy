#pragma once

// --- Settings keys (x64dbg.ini [Rippy] section) ---

constexpr const char* CFG_SECTION    = "Rippy";
constexpr const char* CFG_PROVIDER   = "Provider";
constexpr const char* CFG_API_KEY    = "ApiKey";
constexpr const char* CFG_ENDPOINT   = "Endpoint";
constexpr const char* CFG_MODEL      = "Model";
constexpr const char* CFG_RIPPY_DIR  = "RippyDir";

// --- Provider identifiers ---

constexpr const char* PROVIDER_ANTHROPIC = "anthropic";
constexpr const char* PROVIDER_OPENAI    = "openai";

// --- Default values per provider ---

constexpr const char* DEFAULT_ANTHROPIC_ENDPOINT = "https://api.anthropic.com";
constexpr const char* DEFAULT_ANTHROPIC_MODEL    = "claude-opus-4-6";

constexpr const char* DEFAULT_OPENAI_ENDPOINT    = "https://api.openai.com/v1";
constexpr const char* DEFAULT_OPENAI_MODEL       = "gpt-5.4";

// --- API constants ---

constexpr const char* ANTHROPIC_API_VERSION = "2023-06-01";
