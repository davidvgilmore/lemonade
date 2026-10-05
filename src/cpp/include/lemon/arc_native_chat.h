#pragma once

#include "lemon/arc_chat_frames.h"
#include "lemon/arc_messages.h"

#include <deque>

namespace lemon {

// Only complete native SSE events reach the pinned return codec.
class ArcMessagesFrames {
public:
    template <typename Emit>
    bool accept(const char* data, size_t size, Emit emit) {
        if (bytes_ + size > 16 * 1024 * 1024) return false;
        bytes_ += size;
        pending_.append(data, size);
        size_t end;
        while ((end = pending_.find('\n')) != std::string::npos) {
            std::string line = pending_.substr(0, end + 1);
            pending_.erase(0, end + 1);
            frame_ += line;
            if (line == "\n" || line == "\r\n") {
                if (!observed_.accept(frame_.data(), frame_.size()) ||
                    !emit(frame_, observed_.terminal())) return false;
                frame_.clear();
                if (observed_.terminal()) return true;
            }
        }
        return true;
    }
    bool terminal() const { return observed_.terminal(); }
private:
    size_t bytes_ = 0;
    std::string pending_, frame_;
    ArcMessagesStream observed_;
};

// Count withheld bytes too. Accepted terminal is distinct from parsed terminal:
// the client must receive it after native completion and codec finalization.
class ArcChatCodecDelivery {
public:
    template <typename Write>
    bool deliver(const json& result, bool finished, Write write) {
        if (result.contains("frames")) {
            for (const auto& item : result.at("frames")) {
                const auto frame = item.get<std::string>();
                if (bytes_ + frame.size() > 16 * 1024 * 1024) return false;
                bytes_ += frame.size();
                held_.push_back(frame);
            }
        }
        while (!held_.empty()) {
            auto probe = observed_;
            const auto& frame = held_.front();
            if (!probe.accept(frame.data(), frame.size())) return false;
            if (probe.finished() && !finished) break;
            if (probe.terminal() && held_.size() != 1) return false;
            if (!write(frame.data(), frame.size())) return false;
            observed_ = std::move(probe);
            accepted_ = observed_.terminal() && finished;
            held_.pop_front();
        }
        return true;
    }
    bool accepted_terminal() const { return accepted_; }
    json messages() const { return observed_.messages(); }
private:
    size_t bytes_ = 0;
    std::deque<std::string> held_;
    ArcChatStream observed_;
    bool accepted_ = false;
};

} // namespace lemon
