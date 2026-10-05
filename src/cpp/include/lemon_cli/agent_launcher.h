#pragma once

#include <string>
#include <vector>
#include <utility>

namespace lemon_tray {

struct AgentConfig {
    std::string binary_name;
    std::vector<std::string> binary_alternatives;
    std::vector<std::string> fallback_paths;
    std::vector<std::pair<std::string, std::string>> env_vars;
    std::vector<std::string> extra_args;
    std::string install_instructions;
};

struct AgentLaunchOptions {
    std::string claude_fresh_profile;
    int claude_context_tokens = 0;
    bool codex_use_user_config = false;
    std::string codex_model_provider = "lemonade";
};

bool validate_claude_launch_options(const AgentLaunchOptions& options, std::string& error_message);
bool prepare_claude_profile(const AgentLaunchOptions& options, std::string& error_message);

// Returns true if the agent requires file-based config sync before launch.
bool agent_needs_config_sync(const std::string& agent);

// Build normalized HTTP origin for agent endpoints (e.g. http://localhost:13305).
std::string build_agent_server_base_url(const std::string& host, int port);

// Build launcher configuration for a supported agent.
// Returns true on success, false if agent is unknown.
bool build_agent_config(const std::string& agent,
                        const std::string& host,
                        int port,
                        const std::string& model,
                        const std::string& api_key,
                        const AgentLaunchOptions& launch_options,
                        AgentConfig& config,
                        std::string& error_message);

// Locate the agent executable.
// Returns absolute or PATH-resolved executable for ProcessManager::start_process.
std::string find_agent_binary(const AgentConfig& config);

} // namespace lemon_tray
