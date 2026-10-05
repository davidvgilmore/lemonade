#include "lemon/thinking_controls.h"
#include <stdexcept>
#include <iostream>

void check(bool condition) { if (!condition) throw std::runtime_error("thinking control regression"); }

int main() {
    for (const std::string type : {"enabled", "adaptive", "disabled"}) {
        lemon::json request = {{"messages", {{{"role", "user"}, {"content", "Hello"}}}}};
        const auto messages = request["messages"];
        check(lemon::apply_anthropic_thinking_type(request, type));
        check(request["chat_template_kwargs"]["enable_thinking"] == (type != "disabled"));
        check(request["messages"] == messages);
        if (type == "disabled") check(request["reasoning_effort"] == "none");
        else check(!request.contains("reasoning_effort"));
    }
    lemon::json unknown = {{"model", "unchanged"}};
    const auto before = unknown;
    check(!lemon::apply_anthropic_thinking_type(unknown, "future-mode"));
    check(unknown == before);
    std::cout << "Messages thinking modes preserve backend default effort and reject unknown modes\n";
}
