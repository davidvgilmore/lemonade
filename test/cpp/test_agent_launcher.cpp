#include "lemon_cli/agent_launcher.h"
#include <lemon/utils/process_manager.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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
    if (argc > 1 && std::string(argv[1]) == "--stdio-probe") {
        std::string line;
        std::getline(std::cin, line);
        std::cout << "STDOUT_MARKER:" << line << std::endl;
        std::cerr << "STDERR_MARKER" << std::endl;
        return line == "STDIN_MARKER" && std::cout.good() && std::cerr.good() ? 0 : 3;
    }
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
#ifndef _WIN32
    // Exercise actual spawn inheritance, filtering and output suppression.
    for (int mode = 0; mode < 3; ++mode) {
        std::ofstream(parent / "stdin") << "STDIN_MARKER\n";
        std::cout.flush(); std::cerr.flush();
        const int saved[] = {dup(0), dup(1), dup(2)};
        const int input = open((parent / "stdin").c_str(), O_RDONLY);
        const int output = open((parent / "stdout").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int errors = open((parent / "stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (input < 0 || output < 0 || errors < 0 || saved[0] < 0 || saved[1] < 0 || saved[2] < 0) return 1;
        dup2(input, 0); dup2(output, 1); dup2(errors, 2);
        close(input); close(output); close(errors);
        auto stdio_child = lemon::utils::ProcessManager::start_process(
            fs::absolute(argv[0]).string(), {"--stdio-probe"}, "", mode != 2, mode == 1, {});
        const int stdio_exit = lemon::utils::ProcessManager::wait_for_exit(stdio_child, 5);
        if (stdio_exit == -1) lemon::utils::ProcessManager::kill_process(stdio_child);
        // Filter readers are detached: process exit alone does not prove log drain.
        // Keep the captured descriptors active until both markers arrive or bounded refusal.
        if (mode == 1 && stdio_exit == 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (true) {
                std::ifstream out(parent / "stdout"), err(parent / "stderr");
                const std::string observed = std::string(std::istreambuf_iterator<char>(out), {})
                    + std::string(std::istreambuf_iterator<char>(err), {});
                if (observed.find("STDOUT_MARKER:STDIN_MARKER") != std::string::npos
                    && observed.find("STDERR_MARKER") != std::string::npos) break;
                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        std::cout.flush(); std::cerr.flush();
        for (int fd = 0; fd < 3; ++fd) { dup2(saved[fd], fd); close(saved[fd]); }
        std::cout << "Synthetic stdio child PID " << stdio_child.pid << " exit " << stdio_exit << std::endl;
        std::ifstream stdout_file(parent / "stdout"), stderr_file(parent / "stderr");
        const std::string stdout_text((std::istreambuf_iterator<char>(stdout_file)), {});
        const std::string stderr_text((std::istreambuf_iterator<char>(stderr_file)), {});
        const auto combined = stdout_text + stderr_text;
        if (mode == 2) {
            check("suppressed child output stays suppressed", stdio_exit != -1
                  && combined.find("STDOUT_MARKER") == std::string::npos
                  && combined.find("STDERR_MARKER") == std::string::npos);
        } else {
            check(mode == 0 ? "actual inherited stdin stdout stderr survive spawn"
                            : "filtered output preserves stdin and both output streams", stdio_exit == 0
                  && (mode == 0 ? stdout_text : combined).find("STDOUT_MARKER:STDIN_MARKER") != std::string::npos
                  && (mode == 0 ? stderr_text : combined).find("STDERR_MARKER") != std::string::npos);
        }
    }
#endif
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
