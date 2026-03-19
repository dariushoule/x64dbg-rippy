#pragma once

#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

struct ApiConfig
{
    std::string provider;   // "anthropic" or "openai"
    std::string apiKey;
    std::string endpoint;   // base URL, e.g. "https://api.anthropic.com"
    std::string model;
};

// Blocking call — run on a background thread.
// messages: array of message objects in internal format (Anthropic-style content blocks).
// tools: array of tool definitions (Anthropic format). Pass empty array for no tools.
// system: optional system prompt string (empty = none).
// Returns the full response JSON from the API (caller inspects stop_reason/content).
// Throws std::runtime_error on failure.
nlohmann::json sendChatRaw(
    const ApiConfig& config,
    const nlohmann::json& messages,
    const nlohmann::json& tools = nlohmann::json::array(),
    const std::string& system = ""
);

// Normalize a raw API response into an internal-format assistant message.
// Handles both Anthropic and OpenAI response shapes.
nlohmann::json normalizeResponse(const std::string& provider, const nlohmann::json& raw);

// Extract text content from an internal-format assistant message.
// Returns concatenated text from all "text" content blocks.
std::string extractTextContent(const nlohmann::json& assistantMsg);
