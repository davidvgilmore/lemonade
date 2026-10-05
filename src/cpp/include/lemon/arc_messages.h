#pragma once

#include "lemon/arc_session.h"

#include <set>

namespace lemon {

inline json arc_messages_assistant(const json& response) {
    if (!response.is_object() || response.contains("error") ||
        response.value("type", "") != "message" || response.value("role", "") != "assistant" ||
        !response.contains("content") || !response.at("content").is_array() ||
        !response.contains("stop_reason") || !response.at("stop_reason").is_string() ||
        response.at("stop_reason").get<std::string>().empty()) return nullptr;
    return json::array({{{"role", "assistant"}, {"content", response.at("content")}}});
}

class ArcMessagesStream {
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
            if (line.empty()) {
                if (!event_data_.empty()) {
                    try {
                        const auto first = event_data_.find_first_not_of(" \t\r\n");
                        const auto last = event_data_.find_last_not_of(" \t\r\n");
                        const auto payload = first == std::string::npos ? std::string() :
                            event_data_.substr(first, last - first + 1);
                        if (payload == "[DONE]") {
                            if (!terminal() || done_) valid_ = false;
                            else done_ = true;
                        } else {
                            if (done_) valid_ = false;
                            else event(json::parse(event_data_));
                        }
                    }
                    catch (...) { valid_ = false; }
                    event_data_.clear();
                }
            } else if (line.rfind("data:", 0) == 0) {
                if (!event_data_.empty()) event_data_ += '\n';
                event_data_ += line.substr(5);
            }
        }
        return valid_;
    }
    bool terminal() const { return valid_ && started_ && finished_ && stopped_ && open_.empty(); }
    json messages() const {
        if (!terminal() || !known_) return nullptr;
        json content = json::array();
        int expected = 0;
        for (const auto& [index, block] : blocks_) {
            if (index != expected++) return nullptr;
            content.push_back(block);
        }
        return json::array({{{"role", "assistant"}, {"content", content}}});
    }
private:
    void event(const json& value) {
        const auto type = value.at("type").get<std::string>();
        if (type == "ping") return;
        if (stopped_ || type == "error") { valid_ = false; return; }
        if (type == "message_start") {
            const auto& message = value.at("message");
            if (started_ || message.at("role") != "assistant" ||
                !message.at("content").is_array() || !message.at("content").empty()) { valid_ = false; return; }
            started_ = true;
        } else if (!started_) {
            valid_ = false;
        } else if (type == "content_block_start") {
            int index = value.at("index").get<int>();
            if (index < 0 || index > 1024 || blocks_.count(index)) { valid_ = false; return; }
            const auto& block = value.at("content_block");
            if (!block.is_object() || !block.at("type").is_string()) { valid_ = false; return; }
            blocks_[index] = block;
            open_.insert(index);
        } else if (type == "content_block_delta") {
            int index = value.at("index").get<int>();
            if (!open_.count(index)) { valid_ = false; return; }
            const auto& delta = value.at("delta");
            const auto kind = delta.at("type").get<std::string>();
            if (delta.size() != 2) known_ = false;
            auto& block = blocks_.at(index);
            auto append = [&block, &delta](const char* key) {
                block[key] = block.value(key, std::string()) + delta.at(key).get<std::string>();
            };
            if (kind == "text_delta" && block.at("type") == "text") append("text");
            else if (kind == "thinking_delta" && block.at("type") == "thinking") append("thinking");
            else if (kind == "signature_delta" && block.at("type") == "thinking") append("signature");
            else if (kind == "input_json_delta" &&
                     (block.at("type") == "tool_use" || block.at("type") == "server_tool_use")) {
                input_[index] += delta.at("partial_json").get<std::string>();
            } else known_ = false;
        } else if (type == "content_block_stop") {
            int index = value.at("index").get<int>();
            if (!open_.erase(index)) { valid_ = false; return; }
            if (input_.count(index)) {
                auto input = json::parse(input_.at(index), nullptr, false);
                if (!input.is_object()) known_ = false;
                else blocks_.at(index)["input"] = std::move(input);
            }
        } else if (type == "message_delta") {
            const auto& delta = value.at("delta");
            if (delta.contains("stop_reason") && delta.at("stop_reason").is_string() &&
                !delta.at("stop_reason").get<std::string>().empty()) finished_ = true;
        } else if (type == "message_stop") {
            stopped_ = true;
        } else known_ = false;
    }
    std::string buffer_;
    std::string event_data_;
    size_t bytes_ = 0;
    bool valid_ = true, known_ = true, started_ = false, finished_ = false, stopped_ = false, done_ = false;
    std::map<int, json> blocks_;
    std::map<int, std::string> input_;
    std::set<int> open_;
};

} // namespace lemon
