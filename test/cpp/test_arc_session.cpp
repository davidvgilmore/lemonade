#include "lemon/arc_session.h"
#include "lemon/arc_messages.h"

#include <stdexcept>
#include <iostream>

using namespace lemon;

static void require(bool condition) {
    if (!condition) throw std::runtime_error("ARC session contract assertion failed");
}

int main() {
    ArcChatStream stream;
    const std::string events =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"a\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    for (const char byte : events) require(stream.accept(&byte, 1));
    require(stream.terminal());
    const auto message = stream.messages().at(0);
    require(message.at("tool_calls").at(0).at("function").at("arguments") == "{\"path\":\"a\"}");
    require(message.at("tool_calls").at(0).at("id") == "call-1");

    ArcChatStream unknown;
    const std::string opaque = "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{\"signature\":\"opaque\"}]},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    require(unknown.accept(opaque.data(), opaque.size()));
    require(unknown.terminal() && unknown.messages().is_null());
    ArcChatStream partial;
    const std::string incomplete = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n";
    require(partial.accept(incomplete.data(), incomplete.size()) && !partial.terminal());

    ArcMessagesStream native;
    const std::string native_events =
        "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\",\"content\":[]}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"opaque\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n";
    for (const char byte : native_events) require(native.accept(&byte, 1));
    require(!native.terminal());
    require(native.accept("\n", 1) && native.terminal());
    require(native.messages().at(0).at("content").at(0).at("data") == "opaque");

    std::vector<std::string> operations;
    auto settle = [&operations](const std::string& path, const json& body) {
        require(body.at("owner_id") == "host");
        operations.push_back(path);
        return json{{"state", path.find("/commit") != std::string::npos ? "committed" : "aborted"}};
    };
    {
        ArcPreparedSession receipt({"http://127.0.0.1:9/v1/rayline/arc/session", "host"},
                                    {{"session_token", "token"}}, settle);
    }
    require(operations.size() == 1 && operations.back().find("/abort") != std::string::npos);
    {
        ArcPreparedSession receipt({"http://127.0.0.1:9/v1/rayline/arc/session", "host"},
                                    {{"session_token", "token"}}, settle);
        receipt.finish(true, stream.messages());
        receipt.finish(false);
    }
    require(operations.size() == 2 && operations.back().find("/commit") != std::string::npos);
    std::cout << "ARC session lifecycle and streamed history passed\n";
}
