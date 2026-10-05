#include "lemon_cli/agent_launcher.h"
#include <lemon/utils/process_manager.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(const char* name, bool ok) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}
std::string env(const lemon_tray::AgentConfig& config, const std::string& key) {
    for (const auto& item : config.env_vars) if (item.first == key) return item.second;
    return {};
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--probe") {
        const char* home = std::getenv("HOME");
        const char* cap = std::getenv("CLAUDE_CODE_MAX_CONTEXT_TOKENS");
        const char* profile = std::getenv("CLAUDE_CONFIG_DIR");
        return argc == 5 && std::string(argv[2]) == "--setting-sources" && std::string(argv[3]).empty()
            && home && home == (fs::path(argv[4]) / "home").string()
            && profile && profile == (fs::path(argv[4]) / "claude").string()
            && cap && std::string(cap) == "32768" ? 0 : 2;
    }
    lemon_tray::AgentLaunchOptions options;
    lemon_tray::AgentConfig config;
    std::string error;
    const std::string original_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    auto build = [&]() {
        return lemon_tray::build_agent_config("claude", "127.0.0.1", 13305, "user.Router", "token",
                                             options, config, error);
    };
    check("defaults build without a profile or context override", build() && env(config, "HOME").empty()
          && env(config, "CLAUDE_CODE_MAX_CONTEXT_TOKENS").empty() && config.extra_args.empty());
    const std::vector<std::pair<std::string, std::string>> defaults = {
        {"ANTHROPIC_BASE_URL", "http://127.0.0.1:13305"}, {"ANTHROPIC_AUTH_TOKEN", "token"},
        {"LEMONADE_API_KEY", "token"}, {"ANTHROPIC_DEFAULT_OPUS_MODEL", "user.Router"},
        {"ANTHROPIC_DEFAULT_SONNET_MODEL", "user.Router"}, {"ANTHROPIC_DEFAULT_HAIKU_MODEL", "user.Router"},
        {"CLAUDE_CODE_SUBAGENT_MODEL", "user.Router"}, {"CLAUDE_CODE_ATTRIBUTION_HEADER", "0"},
        {"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1"}
    };
    check("default child overrides are unchanged", config.env_vars == defaults);
    options.claude_context_tokens = 32768;
    check("explicit generic context cap reaches child environment", build()
          && env(config, "CLAUDE_CODE_MAX_CONTEXT_TOKENS") == "32768"
          && env(config, "ANTHROPIC_BASE_URL") == "http://127.0.0.1:13305"
          && env(config, "ANTHROPIC_DEFAULT_SONNET_MODEL") == "user.Router");
    options.claude_context_tokens = -1;
    check("negative context refuses", !build());
    options.claude_context_tokens = 0;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path parent = fs::temp_directory_path() / ("lemonade-agent-test-" + std::to_string(unique));
    if (!fs::create_directory(parent)) return 1;
    const fs::path root = parent / "fresh profile";
    options.claude_fresh_profile = root.string();
    check("building launch config does not create profile", build() && !fs::exists(root));
    check("fresh config routes all client state without changing model", env(config, "HOME") == (root / "home").string()
          && env(config, "CLAUDE_CONFIG_DIR") == (root / "claude").string()
          && env(config, "XDG_STATE_HOME") == (root / "state").string()
          && env(config, "TMPDIR") == (root / "tmp").string()
          && config.extra_args == std::vector<std::string>({"--setting-sources", ""}));
#ifdef _WIN32
    check("Windows profile directories are child scoped", env(config, "USERPROFILE") == (root / "home").string()
          && env(config, "APPDATA") == (root / "config").string()
          && env(config, "LOCALAPPDATA") == (root / "cache").string());
#endif
    check("new profile is created with expected private subdirectories", lemon_tray::prepare_claude_profile(options, error)
          && fs::is_directory(root / "home") && fs::is_directory(root / "claude") && fs::is_directory(root / "tmp"));
    options.claude_context_tokens = 32768;
    check("probe config builds", build());
    auto args = config.extra_args;
    args.insert(args.begin(), "--probe");
    args.push_back(root.string());
    auto child = lemon::utils::ProcessManager::start_process(fs::absolute(argv[0]).string(), args, "", true, false, config.env_vars);
    const int child_exit = lemon::utils::ProcessManager::wait_for_exit(child, 5);
    if (child_exit == -1) lemon::utils::ProcessManager::kill_process(child);
#ifdef _WIN32
    else lemon::utils::ProcessManager::reap_process(child);
#endif
    check("actual child receives empty argv and isolated environment", child_exit == 0);
    std::ofstream(root / "sentinel") << "keep";
    check("reuse refuses and preserves existing files", !lemon_tray::prepare_claude_profile(options, error)
          && fs::file_size(root / "sentinel") == 4);
#ifndef _WIN32
    const fs::path linked = parent / "linked profile";
    fs::create_directory_symlink(root, linked);
    options.claude_fresh_profile = linked.string();
    check("existing symlink refuses without writing its target", !lemon_tray::prepare_claude_profile(options, error)
          && fs::file_size(root / "sentinel") == 4);
    const fs::path dangling = parent / "dangling profile";
    fs::create_directory_symlink(parent / "missing target", dangling);
    options.claude_fresh_profile = dangling.string();
    check("dangling symlink refuses without creating target", !lemon_tray::prepare_claude_profile(options, error)
          && !fs::exists(parent / "missing target"));
#endif
    for (const auto& invalid : {fs::path("relative"), parent / ".." / "escape", parent.root_path()}) {
        options.claude_fresh_profile = invalid.string();
        check("unsafe profile path refuses before creation", !build() && !lemon_tray::prepare_claude_profile(options, error));
    }
    options.claude_fresh_profile = (parent / "absent" / "child").string();
    check("missing parent refuses without creating parents", !lemon_tray::prepare_claude_profile(options, error)
          && !fs::exists(parent / "absent"));
    check("launcher never mutates its parent HOME", original_home == (std::getenv("HOME") ? std::getenv("HOME") : ""));
    fs::remove_all(parent);
    return failures ? 1 : 0;
}
