#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Returns tool definitions in Anthropic format (array of tool objects).
// Each tool has: name, description, input_schema.
nlohmann::json getToolDefinitions();

// Execute a tool by name with given input. Must be called on the GUI thread.
// Returns a JSON object with the tool result.
nlohmann::json executeTool(const std::string& name, const nlohmann::json& input);
