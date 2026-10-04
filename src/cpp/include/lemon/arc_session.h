#pragma once

#include "lemon/arc_router.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace lemon {

inline ArcSessionConfig parse_arc_session(const json& value) {
    arc_require(value.is_object() && value.size() == 2,
                "ARC session requires endpoint and owner_id");
    ArcSessionConfig config{value.at("endpoint").get<std::string>(),
                            value.at("owner_id").get<std::string>()};
    arc_require(std::regex_match(config.endpoint,
        std::regex("http://127\\.0\\.0\\.1:[0-9]{1,5}/(experimental/arc/session|v1/rayline/arc/session)")),
        "ARC session endpoint must be numeric loopback");
    arc_require(!config.owner_id.empty() && config.owner_id.size() <= 256,
                "ARC session owner_id must be nonempty and at most 256 bytes");
    return config;
}

struct ArcSessionIdentity {
    std::string session_id;
    std::string operation_id;
};

class ArcSessionLease {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool busy = false;
        std::atomic<bool> unresolved{false};
    };
public:
    ArcSessionLease(const std::string& key, const std::function<bool()>& cancelled) {
        static std::mutex registry_mutex;
        static std::map<std::string, std::shared_ptr<State>> registry;
        {
            std::lock_guard<std::mutex> guard(registry_mutex);
            for (auto it = registry.begin(); it != registry.end();) {
                if (it->second.use_count() == 1 && !it->second->unresolved) it = registry.erase(it);
                else ++it;
            }
            state_ = registry[key];
            if (!state_) { state_ = std::make_shared<State>(); registry[key] = state_; }
        }
        std::unique_lock<std::mutex> guard(state_->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (state_->busy) {
            arc_require(!cancelled || !cancelled(), "ARC session wait cancelled");
            arc_require(std::chrono::steady_clock::now() < deadline, "ARC session is awaiting settlement");
            state_->changed.wait_for(guard, std::chrono::milliseconds(100));
        }
        arc_require(!state_->unresolved, "ARC session settlement is unresolved; reconcile the service receipt before restarting this host");
        state_->busy = true;
    }
    void mark_unresolved() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->unresolved = true;
    }
    ~ArcSessionLease() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->busy = false;
        state_->changed.notify_all();
    }
    ArcSessionLease(const ArcSessionLease&) = delete;
    ArcSessionLease& operator=(const ArcSessionLease&) = delete;
private:
    std::shared_ptr<State> state_;
};

class ArcPreparedSession {
public:
    using Call = std::function<json(const std::string&, const json&)>;
    ArcPreparedSession(ArcSessionConfig config, json receipt, Call settle,
                       std::shared_ptr<ArcSessionLease> lease = nullptr)
        : config_(std::move(config)), receipt_(std::move(receipt)), settle_(std::move(settle)), lease_(std::move(lease)) {}
    ~ArcPreparedSession() { finish(false); }
    const json& request() const { return receipt_.at("request"); }
    const json& receipt() const { return receipt_; }

    void finish(bool success, const json& messages = nullptr) noexcept {
        if (settled_.exchange(true)) return;
        try {
            json body = {{"owner_id", config_.owner_id},
                         {"session_token", receipt_.at("session_token")}};
            if (success) {
                body["settlement"] = "successful_2xx_terminal_sent";
                body["response_messages"] = messages;
                if (messages.is_null()) body["response_attribution"] = "unknown";
            }
            const auto result = settle_(config_.endpoint + (success ? "/commit" : "/abort"), body);
            arc_require(result.value("state", "") == (success ? "committed" : "aborted"),
                        "ARC session settlement was not acknowledged");
        } catch (...) {
            // The service may have committed even when its acknowledgment was lost.
            if (lease_) lease_->mark_unresolved();
            std::cerr << "[ARC] Settlement acknowledgment unavailable; session blocked pending reconciliation" << std::endl;
        }
        lease_.reset();
    }

private:
    ArcSessionConfig config_;
    json receipt_;
    Call settle_;
    std::shared_ptr<ArcSessionLease> lease_;
    std::atomic<bool> settled_{false};
};

inline json arc_session_prepare_payload(const json& config, const ArcSessionConfig& session,
                                        const ArcSessionIdentity& identity, const json& request,
                                        const std::string& format = "openai_chat") {
    arc_require(!identity.session_id.empty() && identity.session_id.size() <= 256 &&
                !identity.operation_id.empty() && identity.operation_id.size() <= 256,
                "ARC sessions require X-Client-Session-Id and X-Lemonade-Request-Id");
    arc_require(format == "openai_chat" || format == "anthropic_messages", "Unsupported ARC native request format");
    arc_require(!request.contains("arc_context"), "ARC session mode does not accept replay arc_context");
    arc_require(request.contains("messages") && request.at("messages").is_array(),
                "ARC session requires materialized Chat history");
    arc_require(request.value("n", 1) == 1, "ARC sessions support one assistant choice");
    json actions = json::array();
    for (const auto& item : config.at("actions").items()) actions.push_back(item.key());
    return {{"owner_id", session.owner_id}, {"operation_id", identity.operation_id},
            {"metadata", {{"session_id", identity.session_id}}},
            {"request_format", format}, {"request", request},
            {"available_action_ids", actions}};
}

inline void validate_arc_session_receipt(const json& config, const ArcSessionConfig& session,
                                         const json& receipt, const json& source,
                                         const std::string& format = "openai_chat") {
    arc_require(receipt.at("owner_id") == session.owner_id &&
                receipt.at("package_sha256") == config.at("package").at("package_sha256"),
                "ARC session owner/package mismatch");
    arc_require(receipt.at("request_format") == format &&
                receipt.at("source_request_format") == format,
                "ARC session wire format does not match native ingress");
    const auto action = receipt.at("action_id").get<std::string>();
    arc_require(config.at("actions").contains(action), "ARC session selected unbound action");
    const auto& decision = receipt.at("decision");
    arc_require(decision.at("schema_version") == "rayline.arc.policy-decision-response.v1" &&
                decision.at("package") == config.at("package") &&
                decision.at("decision").at("selected_action_id") == action,
                "ARC session decision binding mismatch");
    arc_require(arc_sha256(decision.at("decision").at("selected_arm_id")),
                "ARC session numerical arm identity is invalid");
    const auto& revision = decision.at("encoding").at("session_revision");
    arc_require(revision.is_number_integer() && revision >= 0,
                "ARC session numerical revision must be a nonnegative integer");
    for (const auto* field : {"transaction_id", "session_token", "episode_id_hash", "context_epoch"}) {
        arc_require(receipt.at(field).is_string() && !receipt.at(field).get<std::string>().empty(),
                    "ARC session receipt identity missing");
    }
    const auto& request = receipt.at("request");
    arc_require(request.is_object() && request.at("messages").is_array() &&
                request.at("model") == config.at("actions").at(action).at("wire_model") &&
                request.value("stream", false) == source.value("stream", false) &&
                request.value("n", 1) == 1 && !request.contains("arc_context"),
                "ARC prepared request destination or response mode mismatch");
}

std::shared_ptr<ArcPreparedSession> prepare_arc_session(
    const json& config, const ArcSessionConfig& session, const ArcSessionIdentity& identity,
    const json& request, const std::string& format, std::function<bool()> cancelled = {});

inline json arc_chat_assistant(const json& response) {
    if (response.contains("error") || !response.contains("choices") ||
        !response.at("choices").is_array() || response.at("choices").size() != 1) return nullptr;
    const auto& choice = response.at("choices").at(0);
    if (!choice.contains("finish_reason") || choice.at("finish_reason").is_null() ||
        !choice.contains("message") || choice.at("message").value("role", "") != "assistant") return nullptr;
    return json::array({choice.at("message")});
}

class ArcChatStream {
public:
    bool accept(const char* data, size_t length) {
        if (bytes_ + length > 16 * 1024 * 1024) { valid_ = false; return false; }
        bytes_ += length;
        buffer_.append(data, length);
        size_t end;
        while ((end = buffer_.find('\n')) != std::string::npos) {
            std::string line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;
            std::string data_line = line.substr(5);
            if (!data_line.empty() && data_line.front() == ' ') data_line.erase(0, 1);
            if (data_line == "[DONE]") { done_ = true; continue; }
            try { event(json::parse(data_line)); }
            catch (...) { valid_ = false; }
        }
        return valid_;
    }
    bool terminal() const { return valid_ && done_ && finished_; }
    json messages() const {
        if (!terminal() || !known_) return nullptr;
        json result = assistant_;
        if (!result.contains("content")) result["content"] = nullptr;
        if (!tools_.empty()) {
            result["tool_calls"] = json::array();
            for (const auto& entry : tools_) result["tool_calls"].push_back(entry.second);
        }
        return json::array({result});
    }
private:
    void event(const json& value) {
        if (value.contains("error") || done_) { valid_ = false; return; }
        if (!value.contains("choices")) { known_ = false; return; }
        for (const auto& choice : value.at("choices")) {
            if (choice.value("index", 0) != 0) { valid_ = false; return; }
            if (choice.contains("finish_reason") && !choice.at("finish_reason").is_null()) finished_ = true;
            if (!choice.contains("delta")) continue;
            for (const auto& item : choice.at("delta").items()) {
                const auto& key = item.key(); const auto& part = item.value();
                if (part.is_null()) continue;
                if (key == "role") {
                    if (part != "assistant") valid_ = false;
                } else if (key == "content" || key == "reasoning_content" || key == "refusal") {
                    if (!part.is_string()) { known_ = false; continue; }
                    assistant_[key] = assistant_.value(key, std::string()) + part.get<std::string>();
                } else if (key == "tool_calls") {
                    for (const auto& fragment : part) {
                        int index = fragment.at("index").get<int>();
                        if (index < 0 || index > 1024) { valid_ = false; return; }
                        auto& tool = tools_[index];
                        if (tool.is_null()) tool = json::object();
                        for (const auto& field : fragment.items()) {
                            if (field.key() == "index") continue;
                            if (field.key() == "function") {
                                for (const auto& fn : field.value().items()) {
                                    if (!fn.value().is_string()) { known_ = false; continue; }
                                    auto& function = tool["function"];
                                    if (function.is_null()) function = json::object();
                                    function[fn.key()] = function.value(fn.key(), std::string()) + fn.value().get<std::string>();
                                }
                            } else if (field.key() == "id" || field.key() == "type") {
                                if (tool.contains(field.key()) && tool.at(field.key()) != field.value()) known_ = false;
                                tool[field.key()] = field.value();
                            } else known_ = false;
                        }
                    }
                } else known_ = false;
            }
        }
    }
    std::string buffer_;
    size_t bytes_ = 0;
    bool valid_ = true, known_ = true, done_ = false, finished_ = false;
    json assistant_ = {{"role", "assistant"}};
    std::map<int, json> tools_;
};

} // namespace lemon
