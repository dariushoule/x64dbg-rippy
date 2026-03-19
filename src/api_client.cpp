#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api_client.h"
#include "config.h"

#include <regex>

using json = nlohmann::json;

// Parse scheme, host, port, and path prefix from a URL.
static std::pair<std::string, std::string> parseEndpoint(const std::string& url)
{
    std::regex re(R"(^(https?://[^/]+)(/.*)?)");
    std::smatch m;
    if (std::regex_match(url, m, re))
    {
        std::string base = m[1].str();
        std::string path = m[2].str();
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        return {base, path};
    }
    throw std::runtime_error("Invalid endpoint URL: " + url);
}

// --- Anthropic Messages API ---

// Translate internal message format to Anthropic wire format.
// Internal format already mirrors Anthropic, so this is mostly pass-through.
// The one transformation: tool_result content blocks in a "user" message.
static json toAnthropicMessages(const json& messages)
{
    return messages; // Internal format IS Anthropic format
}

// Translate Anthropic tool definitions (internal format) to Anthropic wire format.
// No transformation needed — internal format IS Anthropic format.
static json toAnthropicTools(const json& tools)
{
    return tools;
}

static json sendAnthropicRaw(
    const std::string& baseUrl,
    const std::string& pathPrefix,
    const ApiConfig& config,
    const json& messages,
    const json& tools,
    const std::string& system)
{
    httplib::Client cli(baseUrl);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);

    json body = {
        {"model", config.model},
        {"max_tokens", 4096},
        {"messages", toAnthropicMessages(messages)}
    };

    if (!tools.empty())
        body["tools"] = toAnthropicTools(tools);

    if (!system.empty())
        body["system"] = system;

    httplib::Headers headers = {
        {"x-api-key", config.apiKey},
        {"anthropic-version", ANTHROPIC_API_VERSION},
        {"content-type", "application/json"}
    };

    std::string path = pathPrefix + "/v1/messages";
    auto res = cli.Post(path, headers, body.dump(), "application/json");

    if (!res)
        throw std::runtime_error("Connection failed: " + httplib::to_string(res.error()));

    if (res->status != 200)
    {
        std::string errMsg = "API error (HTTP " + std::to_string(res->status) + ")";
        try
        {
            auto errBody = json::parse(res->body);
            if (errBody.contains("error") && errBody["error"].contains("message"))
                errMsg += ": " + errBody["error"]["message"].get<std::string>();
        }
        catch (...) {}
        throw std::runtime_error(errMsg);
    }

    return json::parse(res->body);
}

static json normalizeAnthropicResponse(const json& raw)
{
    // Anthropic response: {"content": [...], "stop_reason": "end_turn"|"tool_use", ...}
    // Internal format: {"role": "assistant", "content": [...content blocks...]}
    json msg = {{"role", "assistant"}, {"content", raw["content"]}};
    return msg;
}

// --- OpenAI Chat Completions API ---

// Translate internal messages to OpenAI wire format.
static json toOpenAIMessages(const json& messages, const std::string& system)
{
    json result = json::array();

    // System message first
    if (!system.empty())
        result.push_back({{"role", "system"}, {"content", system}});

    for (const auto& msg : messages)
    {
        const std::string role = msg["role"].get<std::string>();
        const auto& content = msg["content"];

        if (content.is_string())
        {
            // Simple text message
            result.push_back({{"role", role}, {"content", content}});
        }
        else if (content.is_array())
        {
            // Content blocks — need to translate
            if (role == "assistant")
            {
                // Check for tool_use blocks
                json textParts;
                json toolCalls = json::array();
                for (const auto& block : content)
                {
                    if (block["type"] == "text")
                    {
                        if (textParts.is_null()) textParts = "";
                        textParts = textParts.get<std::string>() + block["text"].get<std::string>();
                    }
                    else if (block["type"] == "tool_use")
                    {
                        toolCalls.push_back({
                            {"id", block["id"]},
                            {"type", "function"},
                            {"function", {
                                {"name", block["name"]},
                                {"arguments", block["input"].dump()}
                            }}
                        });
                    }
                }

                json oaiMsg = {{"role", "assistant"}};
                if (!textParts.is_null())
                    oaiMsg["content"] = textParts;
                else
                    oaiMsg["content"] = nullptr;
                if (!toolCalls.empty())
                    oaiMsg["tool_calls"] = toolCalls;
                result.push_back(oaiMsg);
            }
            else if (role == "user")
            {
                // Check for tool_result blocks
                bool hasToolResults = false;
                for (const auto& block : content)
                {
                    if (block["type"] == "tool_result")
                    {
                        hasToolResults = true;
                        result.push_back({
                            {"role", "tool"},
                            {"tool_call_id", block["tool_use_id"]},
                            {"content", block["content"]}
                        });
                    }
                }
                if (!hasToolResults)
                {
                    // Regular user message with content blocks
                    std::string text;
                    for (const auto& block : content)
                    {
                        if (block["type"] == "text")
                            text += block["text"].get<std::string>();
                    }
                    result.push_back({{"role", "user"}, {"content", text}});
                }
            }
        }
    }

    return result;
}

// Translate internal tool definitions to OpenAI function calling format.
static json toOpenAITools(const json& tools)
{
    json result = json::array();
    for (const auto& tool : tools)
    {
        result.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool["name"]},
                {"description", tool["description"]},
                {"parameters", tool["input_schema"]}
            }}
        });
    }
    return result;
}

static json sendOpenAIRaw(
    const std::string& baseUrl,
    const std::string& pathPrefix,
    const ApiConfig& config,
    const json& messages,
    const json& tools,
    const std::string& system)
{
    httplib::Client cli(baseUrl);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);

    json body = {
        {"model", config.model},
        {"messages", toOpenAIMessages(messages, system)}
    };

    if (!tools.empty())
        body["tools"] = toOpenAITools(tools);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + config.apiKey},
        {"content-type", "application/json"}
    };

    std::string path = pathPrefix + "/chat/completions";
    auto res = cli.Post(path, headers, body.dump(), "application/json");

    if (!res)
        throw std::runtime_error("Connection failed: " + httplib::to_string(res.error()));

    if (res->status != 200)
    {
        std::string errMsg = "API error (HTTP " + std::to_string(res->status) + ")";
        try
        {
            auto errBody = json::parse(res->body);
            if (errBody.contains("error") && errBody["error"].contains("message"))
                errMsg += ": " + errBody["error"]["message"].get<std::string>();
        }
        catch (...) {}
        throw std::runtime_error(errMsg);
    }

    return json::parse(res->body);
}

static json normalizeOpenAIResponse(const json& raw)
{
    // OpenAI response: {"choices": [{"message": {"role": "assistant", "content": "...", "tool_calls": [...]}}]}
    const auto& message = raw["choices"][0]["message"];
    json content = json::array();

    if (message.contains("content") && !message["content"].is_null())
    {
        content.push_back({{"type", "text"}, {"text", message["content"]}});
    }

    if (message.contains("tool_calls"))
    {
        for (const auto& tc : message["tool_calls"])
        {
            json input;
            try { input = json::parse(tc["function"]["arguments"].get<std::string>()); }
            catch (...) { input = json::object(); }

            content.push_back({
                {"type", "tool_use"},
                {"id", tc["id"]},
                {"name", tc["function"]["name"]},
                {"input", input}
            });
        }
    }

    return {{"role", "assistant"}, {"content", content}};
}

// --- Public API ---

json sendChatRaw(
    const ApiConfig& config,
    const json& messages,
    const json& tools,
    const std::string& system)
{
    if (config.apiKey.empty())
        throw std::runtime_error("No API key configured. Open Settings from the plugin menu.");

    if (config.endpoint.empty())
        throw std::runtime_error("No endpoint URL configured.");

    auto [baseUrl, pathPrefix] = parseEndpoint(config.endpoint);

    if (config.provider == PROVIDER_ANTHROPIC)
        return sendAnthropicRaw(baseUrl, pathPrefix, config, messages, tools, system);
    else
        return sendOpenAIRaw(baseUrl, pathPrefix, config, messages, tools, system);
}

json normalizeResponse(const std::string& provider, const json& raw)
{
    if (provider == PROVIDER_ANTHROPIC)
        return normalizeAnthropicResponse(raw);
    else
        return normalizeOpenAIResponse(raw);
}

std::string extractTextContent(const json& assistantMsg)
{
    const auto& content = assistantMsg["content"];

    // Simple string content
    if (content.is_string())
        return content.get<std::string>();

    // Array of content blocks — concatenate text blocks
    std::string result;
    if (content.is_array())
    {
        for (const auto& block : content)
        {
            if (block.contains("type") && block["type"] == "text")
            {
                if (!result.empty()) result += "\n";
                result += block["text"].get<std::string>();
            }
        }
    }
    return result;
}
