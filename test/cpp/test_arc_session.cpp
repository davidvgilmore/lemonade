#include "lemon/arc_chat_frames.h"
#include "lemon/arc_messages.h"
#include "lemon/arc_native_chat.h"
#include "lemon/arc_session.h"

#include <iostream>
#include <stdexcept>

using namespace lemon;

static void require(bool condition) {
    if (!condition) throw std::runtime_error("ARC session contract assertion failed");
}

int main() {
    const std::string action(64, 'a'), pin(64, 'b');
    const json config = {{"package", {{"alias", "fixture"}, {"package_sha256", pin}}},
                         {"actions", {{action, {{"wire_model", "native-model"}}}}}};
    const ArcSessionConfig session{"http://127.0.0.1:9/session", "owner", pin};
    const json source = {{"messages", json::array()}, {"model", "router"}, {"stream", false}};
    json receipt = {{"owner_id", "owner"}, {"package_sha256", pin},
        {"source_request_format", "openai_chat"}, {"request_format", "anthropic_messages"},
        {"response_codec", {{"schema_version", "rayline.arc.response-codec.v1"},
            {"source", "anthropic_messages"}, {"target", "openai_chat"}, {"implementation_sha256", pin}}},
        {"action_id", action}, {"transaction_id", "transaction"}, {"session_token", "token"},
        {"episode_id_hash", "episode"}, {"context_epoch", "0"},
        {"request", {{"messages", json::array()}, {"model", "native-model"}}},
        {"decision", {{"schema_version", "rayline.arc.policy-decision-response.v1"},
            {"package", config.at("package")}, {"decision", {{"selected_action_id", action}, {"selected_arm_id", pin}}},
            {"encoding", {{"session_revision", 1}}}}}};
    validate_arc_session_receipt(config, session, receipt, source);
    for (const auto* key : {"source", "target", "implementation_sha256"}) {
        auto wrong = receipt;
        wrong["response_codec"][key] = "wrong";
        bool refused = false;
        try { validate_arc_session_receipt(config, session, wrong, source); }
        catch (const std::exception&) { refused = true; }
        require(refused);
    }
    auto reverse = receipt;
    reverse["source_request_format"] = "anthropic_messages";
    reverse["request_format"] = "openai_chat";
    reverse["response_codec"]["source"] = "openai_chat";
    reverse["response_codec"]["target"] = "anthropic_messages";
    validate_arc_session_receipt(config, session, reverse, source, "anthropic_messages");

    ArcMessagesFrames native_frames;
    const std::string native_complete =
        "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\",\"content\":[]}}\r\n\r\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n";
    int native_count = 0;
    const auto native_emit = [&](const std::string&, bool) { ++native_count; return true; };
    for (const auto byte : native_complete) require(native_frames.accept(&byte, 1, native_emit));
    require(native_count == 2 && !native_frames.terminal());
    require(native_frames.accept("\n", 1, native_emit) && native_frames.terminal() && native_count == 3);

    const std::string final_chat = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"answer\"},\"finish_reason\":\"stop\"}]}\n\n";
    ArcChatCodecDelivery delivery;
    std::string written;
    const auto write = [&](const char* data, size_t size) { written.append(data, size); return true; };
    require(delivery.deliver(json::object(), false, write)); // stream_start may omit frames.
    require(delivery.deliver({{"frames", {final_chat, "data: [DONE]\n\n"}}}, false, write));
    require(!delivery.accepted_terminal() && written.empty());
    require(delivery.deliver({{"frames", json::array()}}, true, write));
    require(delivery.accepted_terminal() && delivery.messages().at(0).at("content") == "answer");
    ArcChatCodecDelivery dropped;
    require(dropped.deliver({{"frames", {final_chat}}}, false, write));
    require(!dropped.deliver({{"frames", {"data: [DONE]\n\n"}}}, true,
        [](const char*, size_t) { return false; }));
    require(!dropped.accepted_terminal());
    ArcChatCodecDelivery trailing;
    require(trailing.deliver({{"frames", {final_chat}}}, false, write));
    written.clear();
    require(!trailing.deliver({{"frames", {"data: [DONE]\n\n", final_chat}}}, true, write));
    require(!trailing.accepted_terminal() && written.find("[DONE]") == std::string::npos);
    ArcChatCodecDelivery oversized;
    require(!oversized.deliver({{"frames", {std::string(16 * 1024 * 1024 + 1, 'x')}}}, false, write));

    ArcChatFrames frames;
    const std::string complete = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n";
    size_t count = 0;
    const auto emit = [&](const std::string&, bool) { ++count; return true; };
    for (const char byte : complete) require(frames.accept(&byte, 1, emit));
    require(count == 1 && !frames.terminal());
    require(frames.accept("\n", 1, emit) && frames.terminal() && count == 2);
    require(parse_arc_session({{"endpoint", "http://127.0.0.1:9/experimental/arc/session"},
                              {"owner_id", "host"}, {"codec_sha256", std::string(64, 'e')}}).codec_sha256.size() == 64);
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
