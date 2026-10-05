#pragma once

#include "lemon/arc_session.h"

namespace lemon {

class ArcChatFrames {
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
                if (!observed_.accept(frame_.data(), frame_.size()) || !emit(frame_, observed_.terminal())) return false;
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
    ArcChatStream observed_;
};

} // namespace lemon
