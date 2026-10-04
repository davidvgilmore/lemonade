#include "lemon/anthropic_conversion.h"
#include <iostream>
#include <stdexcept>
#include <map>

using nlohmann::json;
namespace codec = lemon::anthropic;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<json> stream(const std::vector<json>& chunks, bool fragmented = true) {
    std::string wire;
    httplib::DataSink sink;
    sink.is_writable = [] { return true; };
    sink.write = [&](const char* data, size_t len) { wire.append(data, len); return true; };
    sink.done = [] {};
    codec::stream_openai_sse_to_anthropic_sse("{}", sink, "test-model", {},
        [&](const std::string&, httplib::DataSink& adapter) {
            std::string input;
            for (const auto& chunk : chunks) input += "data: " + chunk.dump() + "\n\n";
            input += "data: [DONE]\n\n";
            const size_t size = fragmented ? 7 : input.size();
            for (size_t i = 0; i < input.size(); i += size) {
                if (!adapter.write(input.data() + i, std::min(size, input.size() - i))) break;
            }
            adapter.done();
        });
    std::vector<json> events;
    size_t pos = 0;
    while ((pos = wire.find("data: ", pos)) != std::string::npos) {
        const size_t end = wire.find('\n', pos);
        events.push_back(json::parse(wire.substr(pos + 6, end - pos - 6)));
        pos = end;
    }
    return events;
}
static json chunk(const json& delta, const json& finish = nullptr) {
    return {{"id", "msg_test"}, {"choices", {{{"delta", delta}, {"finish_reason", finish}}}}};
}
int main() {
    std::vector<std::string> warnings;
    const auto request = json::parse(R"({"model":"test-model","max_tokens":100,"messages":[{"role":"user","content":"Read the counter."},{"role":"assistant","content":[{"type":"thinking","thinking":"Check counter.","signature":"opaque-signature"},{"type":"text","text":"Checking."},{"type":"tool_use","id":"call_1","name":"read","input":{"path":"counter.txt"}}]},{"role":"user","content":[{"type":"tool_result","tool_use_id":"call_1","content":"7"}]}]})");
    const auto converted = codec::convert_anthropic_to_openai_chat(request, warnings);
    check(converted["messages"][1]["reasoning_content"] == "Check counter.", "history reasoning lost");
    check(!converted["messages"][1].contains("signature"), "signature sent to Chat");
    check(converted["messages"][1]["tool_calls"][0]["function"]["arguments"] == "{\"path\":\"counter.txt\"}", "tool arguments changed");
    check(converted["messages"][2] == json({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "7"}}), "tool result changed");
    auto opaque = request;
    opaque["messages"][1]["content"].push_back({{"type", "redacted_thinking"}, {"data", "opaque"}});
    bool refused = false;
    try { codec::convert_anthropic_to_openai_chat(opaque, warnings); } catch (const std::invalid_argument&) { refused = true; }
    check(refused, "opaque reasoning silently lost");

    const json tool = {{"index", 0}, {"id", "call_1"}, {"type", "function"}, {"function", {{"name", "read"}, {"arguments", "{\"path\":"}}}};
    const auto events = stream({chunk({{"reasoning_content", "Check "}}), chunk({{"reasoning_content", "counter."}}),
        chunk({{"content", "Checking."}}), chunk({{"tool_calls", {tool}}}),
        chunk(json::parse(R"({"tool_calls":[{"index":0,"function":{"arguments":"\"counter.txt\"}"}}]})")),
        chunk(json::object(), "tool_calls"), {{"choices", json::array()}, {"usage", {{"prompt_tokens", 7}, {"completion_tokens", 9}}}}});
    std::map<int, json> blocks;
    std::map<int, std::string> arguments;
    std::map<int, bool> stopped;
    bool terminal = false;
    for (const auto& event : events) {
        const std::string type = event["type"];
        if (type == "content_block_start") { int i = event["index"]; check(!blocks.count(i), "duplicate block index"); blocks[i] = event["content_block"]; }
        if (type == "content_block_delta") {
            int i = event["index"]; check(blocks.count(i) && !stopped[i], "delta outside open block");
            const auto& delta = event["delta"];
            if (delta["type"] == "thinking_delta") blocks[i]["thinking"] = blocks[i]["thinking"].get<std::string>() + delta["thinking"].get<std::string>();
            if (delta["type"] == "text_delta") blocks[i]["text"] = blocks[i]["text"].get<std::string>() + delta["text"].get<std::string>();
            if (delta["type"] == "input_json_delta") arguments[i] += delta["partial_json"].get<std::string>();
        }
        if (type == "content_block_stop") { int i = event["index"]; check(blocks.count(i) && !stopped[i], "invalid block stop"); stopped[i] = true; }
        if (type == "message_stop") terminal = true;
    }
    check(terminal && blocks.size() == 3, "reasoning/text/tool stream missing blocks");
    check(blocks[0] == json({{"type", "thinking"}, {"thinking", "Check counter."}}), "thinking output or signature invented");
    check(blocks[1] == json({{"type", "text"}, {"text", "Checking."}}), "text output changed");
    check(json::parse(arguments[2]) == json({{"path", "counter.txt"}}), "split tool JSON changed");
    check(stopped[0] && stopped[1] && stopped[2], "unclosed blocks");
    const auto buffered = codec::convert_openai_chat_to_anthropic({{"choices", {{{"message", {
        {"reasoning_content", "Check counter."}, {"content", "Checking."}, {"tool_calls", {{{"id", "call_1"}, {"function", {{"name", "read"}, {"arguments", "{\"path\":\"counter.txt\"}"}}}}}}
    }}, {"finish_reason", "tool_calls"}}}}}, "test-model", {});
    check(buffered["content"][0] == blocks[0] && buffered["content"][1] == blocks[1], "buffered/streaming reasoning mismatch");
    check(buffered["content"][2]["input"] == json::parse(arguments[2]), "buffered/streaming tools mismatch");
    for (const auto& bad : {stream({chunk({{"content", "partial"}})}), stream({{{"error", {{"message", "failed"}}}}})}) {
        bool error = false;
        for (const auto& event : bad) { check(event["type"] != "message_stop", "failure became successful terminal"); error |= event["type"] == "error"; }
        check(error, "failure missing error event");
    }
    std::cout << "Messages thinking/tool history and fragmented streaming verified\n";
}
