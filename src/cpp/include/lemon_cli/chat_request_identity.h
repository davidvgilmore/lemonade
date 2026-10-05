#pragma once

#include <map>
#include <random>
#include <string>

namespace lemon_cli {

class ChatRequestIdentity {
public:
    ChatRequestIdentity() { reset(); }

    void reset() { session_id_ = new_id(); }

    std::map<std::string, std::string> next() const {
        return {{"X-Client-Session-Id", session_id_},
                {"X-Lemonade-Request-Id", new_id()}};
    }

private:
    std::string session_id_;

    static std::string new_id() {
        std::random_device random;
        std::uniform_int_distribution<unsigned int> byte(0, 255);
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(32);
        for (int i = 0; i < 16; ++i) {
            const auto value = byte(random);
            result += hex[value >> 4];
            result += hex[value & 15];
        }
        return result;
    }
};

}
