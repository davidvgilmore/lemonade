#pragma once

#include <nlohmann/json.hpp>

#include <functional>

namespace lemon {

class PreparedRequestScope {
public:
    explicit PreparedRequestScope(const nlohmann::json* request,
                                  std::function<bool()> terminal = {})
        : previous_(current_), previous_terminal_(std::move(terminal_)) {
        current_ = request;
        terminal_ = std::move(terminal);
    }
    ~PreparedRequestScope() { current_ = previous_; terminal_ = std::move(previous_terminal_); }
    PreparedRequestScope(const PreparedRequestScope&) = delete;
    PreparedRequestScope& operator=(const PreparedRequestScope&) = delete;
    static const nlohmann::json* current() { return current_; }
    static bool terminal_delivered() { return current_ && terminal_ && terminal_(); }

private:
    const nlohmann::json* previous_;
    std::function<bool()> previous_terminal_;
    inline static thread_local std::function<bool()> terminal_;
    inline static thread_local const nlohmann::json* current_ = nullptr;
};

} // namespace lemon
