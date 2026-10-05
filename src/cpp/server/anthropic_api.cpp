#include "lemon/arc_chat_frames.h"
#include "lemon/arc_messages.h"
#include "lemon/prepared_request.h"
#include "lemon/anthropic_error.h"
#include "lemon/anthropic_conversion.h"
#include "lemon/anthropic_relay_headers.h"
#include "lemon/anthropic_upstream.h"
#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/error_types.h"
#include "lemon/ollama_api.h"
#include "lemon/thinking_controls.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/session_utils.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace lemon {

namespace {

static void forward_arc_chat(Router* router, json routing_request,
                             const std::shared_ptr<ArcPreparedSession>& prepared,
                             httplib::Response& res, std::function<bool()> cancelled) {
    if (!prepared->request().value("stream", false)) {
        PreparedRequestScope scope(&prepared->request());
        utils::RequestCancelToken cancellation;
        cancellation.should_cancel = cancelled;
        WrappedServer::RequestCancelScope cancel_scope(cancellation);
        const auto response = router->chat_completion(routing_request);
        arc_require(!arc_chat_assistant(response).is_null(), "ARC Chat provider did not return terminal success");
        const auto translated = prepared->transform("response", {{"body", response}}, cancelled).at("body");
        const auto messages = arc_messages_assistant(translated);
        arc_require(!messages.is_null(), "ARC codec did not return a terminal Messages response");
        auto body = std::make_shared<std::string>(translated.dump());
        res.set_content_provider(body->size(), "application/json",
            [body](size_t offset, size_t length, httplib::DataSink& sink) {
                return sink.write(body->data() + offset, length);
            }, [prepared, messages](bool success) { prepared->finish(success, messages); });
        return;
    }
    prepared->transform("stream_start", json::object(), cancelled);
    auto native = std::make_shared<ArcMessagesStream>();
    auto accepted = std::make_shared<bool>(false);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider("text/event-stream",
        [router, routing_request, prepared, cancelled, native, accepted](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) return false;
            try {
                ArcChatFrames provider;
                uint64_t sequence = 0;
                std::vector<std::string> held;
                const auto stopped = [&]() {
                    return (cancelled && cancelled()) || (sink.is_writable && !sink.is_writable());
                };
                auto deliver = [&](const json& result, bool terminal) {
                    for (const auto& item : result.at("frames")) held.push_back(item.get<std::string>());
                    for (auto it = held.begin(); it != held.end();) {
                        // A codec may produce its terminal before the provider's [DONE].
                        // Retain that frame until the provider protocol is complete.
                        auto probe = *native;
                        if (!probe.accept(it->data(), it->size())) return false;
                        if (probe.terminal() && !terminal) break;
                        if (stopped() || !sink.write(it->data(), it->size())) return false;
                        *native = std::move(probe);
                        *accepted = native->terminal() && terminal;
                        it = held.erase(it);
                    }
                    return true;
                };
                PreparedRequestScope scope(&prepared->request(), [accepted]() { return *accepted; });
                httplib::DataSink bridge;
                bridge.is_writable = [&]() { return !stopped(); };
                bridge.done = []() {};
                bridge.done_with_trailer = [](const httplib::Headers&) {};
                bridge.write = [&](const char* data, size_t size) {
                    return provider.accept(data, size, [&](const std::string& frame, bool terminal) {
                        auto result = prepared->transform("stream_push", {{"sequence", sequence++}, {"frame", frame}}, stopped);
                        if (!deliver(result, terminal)) return false;
                        if (terminal) {
                            result = prepared->transform("stream_finish", {{"sequence", sequence++}}, stopped);
                            if (!deliver(result, true)) return false;
                            arc_require(*accepted, "ARC codec did not produce a terminal Messages event");
                        }
                        return true;
                    });
                };
                router->chat_completion_stream(routing_request.dump(), bridge);
                if (*accepted) sink.done();
            } catch (const std::exception&) {
                // Releaser aborts a failed translation without inventing a terminal.
            }
            return *accepted;
        }, [prepared, native, accepted](bool) { prepared->finish(*accepted, native->messages()); });
}

static void add_warning(std::vector<std::string>& warnings, const std::string& warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

// Chat prompt totals include cache hits and writes; Messages input_tokens do not.
// A missing cache partition stays unknown instead of becoming a zero count.
static json anthropic_usage_from_chat(const json& source, std::vector<std::string>& warnings) {
    json result = json::object();
    auto count = [](const json& object, const char* key) -> std::optional<int64_t> {
        if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer()) return std::nullopt;
        const auto value = object[key].get<int64_t>();
        return value >= 0 ? std::optional<int64_t>(value) : std::nullopt;
    };
    const auto total = count(source, "prompt_tokens");
    const auto output = count(source, "completion_tokens");
    const auto details = source.is_object() ? source.value("prompt_tokens_details", json::object()) : json::object();
    const auto read = count(details, "cached_tokens");
    const auto write = count(details, "cache_write_tokens");
    if (output) result["output_tokens"] = *output;
    if (read) result["cache_read_input_tokens"] = *read;
    if (write) result["cache_creation_input_tokens"] = *write;
    if (total && read && write && *read <= *total && *write <= *total - *read) {
        result["input_tokens"] = *total - *read - *write;
    } else {
        add_warning(warnings, "Backend cache breakdown unavailable or inconsistent; uncached input_tokens remains absent");
    }
    if (!output) add_warning(warnings, "Backend output token count unavailable; output_tokens remains absent");
    return result;
}

static std::string join_strings(const std::vector<std::string>& parts, const char* sep = "\n") {
    std::ostringstream os;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) os << sep;
        os << parts[i];
    }
    return os.str();
}

static std::string join_text_blocks(const json& value, std::vector<std::string>& warnings, const std::string& field_name) {
    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (!value.is_array()) {
        add_warning(warnings, "Ignored non-string/non-array '" + field_name + "' field");
        return "";
    }

    std::vector<std::string> parts;
    for (const auto& block : value) {
        if (!block.is_object()) {
            add_warning(warnings, "Ignored non-object block in '" + field_name + "'");
            continue;
        }

        std::string type = block.value("type", "");
        if (type == "text" && block.contains("text") && block["text"].is_string()) {
            parts.push_back(block["text"].get<std::string>());
            continue;
        }

        add_warning(warnings, "Ignored unsupported '" + field_name + "' block type: " + type);
    }

    return join_strings(parts);
}

static std::string map_finish_reason_to_anthropic_stop_reason(const json& choice) {
    std::string finish_reason = choice.value("finish_reason", "stop");

    if (finish_reason == "length") {
        return "max_tokens";
    }
    if (finish_reason == "tool_calls") {
        return "tool_use";
    }
    return "end_turn";
}

static std::string generate_anthropic_message_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "msg_" + std::to_string(millis);
}

static bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string payload = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(payload.c_str(), payload.size());
}

static std::string stringify_anthropic_tool_result_content(const json& content,
                                                           std::vector<std::string>& warnings) {
    if (content.is_string()) {
        return content.get<std::string>();
    }

    if (content.is_array()) {
        std::vector<std::string> parts;
        for (const auto& block : content) {
            if (!block.is_object()) {
                add_warning(warnings, "Ignored non-object block in tool_result.content");
                continue;
            }

            std::string type = block.value("type", "");
            if (type == "text" && block.contains("text") && block["text"].is_string()) {
                parts.push_back(block["text"].get<std::string>());
                continue;
            }

            add_warning(warnings, "Ignored unsupported block type in tool_result.content: " + type);
        }

        return join_strings(parts);
    }

    if (content.is_object()) {
        return content.dump();
    }

    add_warning(warnings, "Ignored unsupported type for tool_result.content");
    return "";
}

static json parse_openai_tool_arguments(const json& tool_call, std::vector<std::string>& warnings) {
    if (!tool_call.is_object() || !tool_call.contains("function") || !tool_call["function"].is_object()) {
        return json::object();
    }

    const auto& fn = tool_call["function"];
    if (!fn.contains("arguments")) {
        return json::object();
    }

    if (fn["arguments"].is_object()) {
        return fn["arguments"];
    }

    if (fn["arguments"].is_string()) {
        const std::string args_str = fn["arguments"].get<std::string>();
        if (args_str.empty()) {
            return json::object();
        }

        try {
            auto parsed = json::parse(args_str);
            if (parsed.is_object()) {
                return parsed;
            }
            add_warning(warnings, "Tool arguments were not an object; wrapped as _value");
            return json{{"_value", parsed}};
        } catch (...) {
            add_warning(warnings, "Failed to parse tool arguments as JSON; wrapped as _raw");
            return json{{"_raw", args_str}};
        }
    }

    add_warning(warnings, "Tool arguments had unsupported type; using empty object");
    return json::object();
}

static bool set_anthropic_backend_error_response(const json& response, httplib::Response& res) {
    if (!response.contains("error")) {
        return false;
    }

    const auto& error = response["error"];
    std::cerr << "[OllamaApi] Backend returned error: " << error.dump() << std::endl;

    const int status = anthropic::backend_error_http_status(error);
    res.status = status;
    res.set_content(anthropic::build_anthropic_error(error, status).dump(), "application/json");
    return true;
}

static void set_anthropic_residency_conflict_response(
    const RouterResidencyConflictException& error,
    httplib::Response& res) {
    res.status = 409;
    json body = {
        {"type", "error"},
        {"error", {
            {"type", ErrorType::ROUTER_RESIDENCY_CONFLICT},
            {"message", error.what()},
        }},
    };
    res.set_content(body.dump(), "application/json");
}

// Everything needed to relay one /v1/messages request to a provider that
// speaks the Anthropic Messages wire format natively.
static void set_anthropic_error_response(httplib::Response& res, int status,
                                         const std::string& message) {
    res.status = status;
    res.set_content(
        anthropic::build_anthropic_error(json{{"message", message}}, status).dump(),
        "application/json");
}


}  // namespace

void OllamaApi::register_anthropic_routes(httplib::Server& server, const std::shared_ptr<OllamaApi>& self) {
    server.Post("/v1/messages", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_messages(req, res);
    });
}

json anthropic::convert_anthropic_to_openai_chat(const json& anthropic_request, std::vector<std::string>& warnings) {
    json openai_req;

    std::string model = anthropic_request.value("model", "");
    const std::string suffix = ":latest";
    if (model.size() > suffix.size() && model.compare(model.size() - suffix.size(), suffix.size(), suffix) == 0) {
        model.resize(model.size() - suffix.size());
    }
    openai_req["model"] = model;

    json messages = json::array();

    if (anthropic_request.contains("system")) {
        std::string system_text = join_text_blocks(anthropic_request["system"], warnings, "system");
        if (!system_text.empty()) {
            messages.push_back({{"role", "system"}, {"content", system_text}});
        }
    }

    if (anthropic_request.contains("messages") && anthropic_request["messages"].is_array()) {
        for (const auto& msg : anthropic_request["messages"]) {
            if (!msg.is_object()) {
                add_warning(warnings, "Ignored non-object item in 'messages'");
                continue;
            }

            std::string role = msg.value("role", "user");
            if (role != "user" && role != "assistant" && role != "system") {
                add_warning(warnings, "Unsupported role '" + role + "' mapped to 'user'");
                role = "user";
            }

            if (role == "system") {
                add_warning(warnings, "Ignored 'system' role in messages; use top-level system field");
                continue;
            }

            if (msg.contains("content") && msg["content"].is_string()) {
                messages.push_back({
                    {"role", role},
                    {"content", msg["content"]}
                });
                continue;
            }

            json content_parts = json::array();
            std::vector<std::string> text_parts;
            std::string reasoning_text;
            bool has_non_text = false;
            json assistant_tool_calls = json::array();
            std::vector<json> tool_result_messages;

            if (msg.contains("content") && msg["content"].is_array()) {
                for (const auto& block : msg["content"]) {
                    if (!block.is_object()) {
                        add_warning(warnings, "Ignored non-object message content block");
                        continue;
                    }

                    std::string type = block.value("type", "");
                    if (type == "text" && block.contains("text") && block["text"].is_string()) {
                        const std::string text = block["text"].get<std::string>();
                        content_parts.push_back({{"type", "text"}, {"text", text}});
                        text_parts.push_back(text);
                        continue;
                    }

                    if (type == "thinking" && role == "assistant" &&
                        block.contains("thinking") && block["thinking"].is_string()) {
                        reasoning_text += block["thinking"].get<std::string>();
                        if (block.contains("signature")) {
                            add_warning(warnings, "Removed thinking signature for OpenAI-compatible backend; reasoning text retained");
                        }
                        continue;
                    }
                    if (type == "redacted_thinking") {
                        throw std::invalid_argument("Cannot translate opaque redacted_thinking to an OpenAI-compatible backend");
                    }

                    if (type == "image" && block.contains("source") && block["source"].is_object()) {
                        const auto& source = block["source"];
                        std::string source_type = source.value("type", "");
                        std::string media_type = source.value("media_type", "");
                        std::string data = source.value("data", "");

                        if (source_type == "base64" && !media_type.empty() && !data.empty()) {
                            has_non_text = true;
                            content_parts.push_back({
                                {"type", "image_url"},
                                {"image_url", {{"url", "data:" + media_type + ";base64," + data}}}
                            });
                            continue;
                        }

                        add_warning(warnings, "Ignored image block with unsupported source format");
                        continue;
                    }

                    if (type == "tool_use") {
                        if (role != "assistant") {
                            add_warning(warnings, "Ignored tool_use block outside assistant role");
                            continue;
                        }

                        std::string tool_name = block.value("name", "");
                        if (tool_name.empty()) {
                            add_warning(warnings, "Ignored tool_use block missing name");
                            continue;
                        }

                        std::string tool_id = block.value("id", generate_anthropic_message_id());
                        json input_obj = json::object();
                        if (block.contains("input")) {
                            if (block["input"].is_object()) {
                                input_obj = block["input"];
                            } else {
                                add_warning(warnings, "tool_use.input was not an object; wrapped as _value");
                                input_obj = json{{"_value", block["input"]}};
                            }
                        }

                        assistant_tool_calls.push_back({
                            {"id", tool_id},
                            {"type", "function"},
                            {"function", {
                                {"name", tool_name},
                                {"arguments", input_obj.dump()}
                            }}
                        });
                        continue;
                    }

                    if (type == "tool_result") {
                        if (role != "user") {
                            add_warning(warnings, "Ignored tool_result block outside user role");
                            continue;
                        }

                        std::string tool_use_id = block.value("tool_use_id", "");
                        if (tool_use_id.empty()) {
                            add_warning(warnings, "Ignored tool_result block missing tool_use_id");
                            continue;
                        }

                        std::string tool_content = block.contains("content")
                            ? stringify_anthropic_tool_result_content(block["content"], warnings)
                            : std::string();

                        if (!content_parts.empty()) {
                            tool_result_messages.push_back({{"role", "user"}, {"content", has_non_text ? content_parts : json(join_strings(text_parts))}});
                            content_parts = json::array();
                            text_parts.clear();
                            has_non_text = false;
                        }
                        tool_result_messages.push_back({
                            {"role", "tool"},
                            {"tool_call_id", tool_use_id},
                            {"content", tool_content}
                        });
                        continue;
                    }

                    add_warning(warnings, "Ignored unsupported message content block type: " + type);
                }
            } else if (msg.contains("content")) {
                add_warning(warnings, "Ignored message content with unsupported type");
            }

            json openai_msg;
            openai_msg["role"] = role;

            if (!content_parts.empty()) {
                if (!has_non_text) {
                    openai_msg["content"] = join_strings(text_parts);
                } else {
                    openai_msg["content"] = content_parts;
                }
            } else {
                openai_msg["content"] = "";
            }

            if (!reasoning_text.empty()) {
                openai_msg["reasoning_content"] = reasoning_text;
            }
            if (!assistant_tool_calls.empty()) {
                openai_msg["tool_calls"] = assistant_tool_calls;
            }

            for (const auto& tool_msg : tool_result_messages) {
                messages.push_back(tool_msg);
            }
            bool has_content = !content_parts.empty();
            bool has_tool_calls = !assistant_tool_calls.empty();

            bool is_tool_result_only = (role == "user" && !has_content && !has_tool_calls && !tool_result_messages.empty());
            if (!is_tool_result_only && (role == "assistant" || has_content || has_tool_calls)) {
                messages.push_back(openai_msg);
            }


        }
    }

    openai_req["messages"] = messages;

    if (anthropic_request.contains("max_tokens")) {
        openai_req["max_completion_tokens"] = anthropic_request["max_tokens"];
    }
    if (anthropic_request.contains("temperature")) {
        openai_req["temperature"] = anthropic_request["temperature"];
    }
    if (anthropic_request.contains("top_p")) {
        openai_req["top_p"] = anthropic_request["top_p"];
    }
    if (anthropic_request.contains("top_k")) {
        openai_req["top_k"] = anthropic_request["top_k"];
    }

    if (anthropic_request.contains("stop_sequences")) {
        openai_req["stop"] = anthropic_request["stop_sequences"];
    }

    if (anthropic_request.contains("tools") && anthropic_request["tools"].is_array()) {
        json openai_tools = json::array();
        for (const auto& tool : anthropic_request["tools"]) {
            if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) {
                add_warning(warnings, "Ignored invalid tool definition in 'tools'");
                continue;
            }

            json parameters = json::object();
            if (tool.contains("input_schema") && tool["input_schema"].is_object()) {
                parameters = tool["input_schema"];
            }

            openai_tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool["name"]},
                    {"description", tool.value("description", "")},
                    {"parameters", parameters}
                }}
            });
        }

        if (!openai_tools.empty()) {
            openai_req["tools"] = openai_tools;
        }
    }

    if (anthropic_request.contains("tool_choice") && anthropic_request["tool_choice"].is_object()) {
        const auto& tc = anthropic_request["tool_choice"];
        std::string type = tc.value("type", "auto");
        if (type == "auto") {
            openai_req["tool_choice"] = "auto";
        } else if (type == "any") {
            openai_req["tool_choice"] = "required";
        } else if (type == "none") {
            openai_req["tool_choice"] = "none";
        } else if (type == "tool") {
            std::string name = tc.value("name", "");
            if (name.empty()) {
                add_warning(warnings, "Ignored tool_choice.type=tool without name");
            } else {
                openai_req["tool_choice"] = {
                    {"type", "function"},
                    {"function", {{"name", name}}}
                };
            }
        } else {
            add_warning(warnings, "Ignored unsupported tool_choice.type: " + type);
        }
    }

    if (anthropic_request.contains("output_config") && anthropic_request["output_config"].is_object()) {
        const auto& output_config = anthropic_request["output_config"];
        if (output_config.contains("format") && output_config["format"].is_object()) {
            const auto& format = output_config["format"];
            std::string type = format.value("type", "");

            if (type == "json_schema" && format.contains("schema") && format["schema"].is_object()) {
                openai_req["response_format"] = {
                    {"type", "json_schema"},
                    {"json_schema", {
                        {"name", "response"},
                        {"schema", format["schema"]}
                    }}
                };
            } else if (type == "json_object") {
                openai_req["response_format"] = { {"type", "json_object"} };
            } else {
                add_warning(warnings, "Ignored unsupported output_config.format type: " + type);
            }
        }
    }

    if (anthropic_request.contains("thinking") && anthropic_request["thinking"].is_object()) {
        std::string thinking_type = anthropic_request["thinking"].value("type", "");
        if (!apply_anthropic_thinking_type(openai_req, thinking_type)) {
            add_warning(warnings, "Ignored unsupported thinking.type: " + thinking_type);
        } else if (thinking_type == "adaptive") {
            add_warning(warnings, "Adaptive thinking enables backend reasoning at its default effort; Anthropic adaptive budgeting is not emulated");
        }
    }

    if (anthropic_request.contains("metadata")) {
        add_warning(warnings, "Ignored 'metadata' field");
    }
    if (anthropic_request.contains("context_management")) {
        add_warning(warnings, "Ignored 'context_management' field");
    }

    openai_req["stream"] = anthropic_request.value("stream", false);

    return openai_req;
}

json anthropic::convert_openai_chat_to_anthropic(const json& openai_response,
                                                 const std::string& model,
                                                 const std::vector<std::string>& warnings) {
    std::vector<std::string> mutable_warnings = warnings;
    std::string response_text;
    std::string response_reasoning;
    json content_blocks = json::array();
    std::string stop_reason = "end_turn";
    std::string response_id = openai_response.value("id", generate_anthropic_message_id());

    if (openai_response.contains("choices") && openai_response["choices"].is_array() &&
        !openai_response["choices"].empty()) {
        const auto& choice = openai_response["choices"][0];
        stop_reason = map_finish_reason_to_anthropic_stop_reason(choice);

        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                response_reasoning = message["reasoning_content"].get<std::string>();
            } else if (message.contains("reasoning") && message["reasoning"].is_string()) {
                response_reasoning = message["reasoning"].get<std::string>();
            }
            if (message.contains("content") && message["content"].is_string()) {
                response_text = message["content"].get<std::string>();
            } else if (message.contains("content") && message["content"].is_array()) {
                std::vector<std::string> text_blocks;
                for (const auto& block : message["content"]) {
                    if (block.is_object() && block.value("type", "") == "text" &&
                        block.contains("text") && block["text"].is_string()) {
                        text_blocks.push_back(block["text"].get<std::string>());
                    }
                }
                response_text = join_strings(text_blocks);
            }

            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& tool_call : message["tool_calls"]) {
                    if (!tool_call.is_object()) {
                        continue;
                    }

                    std::string tool_id = tool_call.value("id", std::string());
                    if (tool_id.empty()) throw std::invalid_argument("tool response missing call ID");
                    std::string tool_name;
                    if (tool_call.contains("function") && tool_call["function"].is_object()) {
                        tool_name = tool_call["function"].value("name", "");
                    }
                    if (tool_name.empty()) {
                        add_warning(mutable_warnings, "Encountered tool_call without function name");
                        continue;
                    }

                    content_blocks.push_back({
                        {"type", "tool_use"},
                        {"id", tool_id},
                        {"name", tool_name},
                        {"input", parse_openai_tool_arguments(tool_call, mutable_warnings)}
                    });
                }
            }
        }
    }

    if (!response_text.empty() || content_blocks.empty()) {
        json merged_blocks = json::array();
        merged_blocks.push_back({
            {"type", "text"},
            {"text", response_text}
        });
        for (const auto& block : content_blocks) {
            merged_blocks.push_back(block);
        }
        content_blocks = merged_blocks;
    }

    if (!response_reasoning.empty()) {
        json with_reasoning = json::array();
        with_reasoning.push_back({{"type", "thinking"}, {"thinking", response_reasoning}});
        for (const auto& block : content_blocks) with_reasoning.push_back(block);
        content_blocks = std::move(with_reasoning);
    }

    if (stop_reason == "end_turn") {
        for (const auto& block : content_blocks) {
            if (block.is_object() && block.value("type", "") == "tool_use") {
                stop_reason = "tool_use";
                break;
            }
        }
    }

    const json usage = anthropic_usage_from_chat(openai_response.value("usage", json::object()), mutable_warnings);

    json anthropic_res = {
        {"id", response_id},
        {"type", "message"},
        {"role", "assistant"},
        {"model", model},
        {"content", content_blocks},
        {"stop_reason", stop_reason},
        {"stop_sequence", nullptr},
        {"usage", usage}
    };

    if (!mutable_warnings.empty()) {
        anthropic_res["warnings"] = mutable_warnings;
    }

    return anthropic_res;
}

void anthropic::stream_openai_sse_to_anthropic_sse(const std::string& openai_body,
                                                   httplib::DataSink& client_sink,
                                                   const std::string& model,
                                                   const std::vector<std::string>& warnings,
                                                   anthropic::StreamFn call_router) {
    httplib::DataSink adapter_sink;
    std::string buffer, stop_reason = "end_turn";
    bool started = false, failed = false, finished = false;
    int next_index = 0, active_index = -1;
    std::string active_kind;
    json usage = json::object();
    struct ToolBlock { int index; std::string id; std::string name; };
    std::map<int, ToolBlock> tools;
    auto emit = [&](const std::string& event, const json& data) {
        if (failed) return false;
        if (!write_sse_event(client_sink, event, data)) { failed = true; return false; }
        return true;
    };
    auto close_active = [&]() {
        if (active_index < 0) return true;
        if (!emit("content_block_stop", {{"type", "content_block_stop"}, {"index", active_index}})) return false;
        active_index = -1;
        active_kind.clear();
        return true;
    };
    auto fail = [&](const std::string& message) {
        if (!failed) write_sse_event(client_sink, "error", {{"type", "error"}, {"error", {{"type", "api_error"}, {"message", message}}}});
        failed = true;
    };
    auto content_delta = [&](const std::string& kind, const std::string& text) {
        if (text.empty()) return true;
        if (active_kind != kind) {
            if (!close_active()) return false;
            active_kind = kind;
            active_index = next_index++;
            const std::string field = kind == "thinking" ? "thinking" : "text";
            if (!emit("content_block_start", {{"type", "content_block_start"}, {"index", active_index}, {"content_block", {{"type", kind}, {field, ""}}}})) return false;
        }
        const std::string field = kind == "thinking" ? "thinking" : "text";
        return emit("content_block_delta", {{"type", "content_block_delta"}, {"index", active_index}, {"delta", {{"type", kind == "thinking" ? "thinking_delta" : "text_delta"}, {field, text}}}});
    };
    adapter_sink.is_writable = client_sink.is_writable;
    adapter_sink.write = [&](const char* data, size_t len) {
        if (failed) return false;
        buffer.append(data, len);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;
            std::string payload = line.substr(5);
            if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
            if (payload == "[DONE]") continue;
            try {
                const auto chunk = json::parse(payload);
                if (chunk.contains("error")) { fail(chunk["error"].dump()); return false; }
                if (!started) {
                    if (!emit("message_start", {{"type", "message_start"}, {"message", {
                        {"id", chunk.value("id", generate_anthropic_message_id())}, {"type", "message"},
                        {"role", "assistant"}, {"model", model}, {"content", json::array()},
                        {"stop_reason", nullptr}, {"stop_sequence", nullptr},
                        {"usage", json::object()}
                    }}})) return false;
                    started = true;
                }
                if (chunk.contains("usage") && chunk["usage"].is_object()) {
                    usage.update(chunk["usage"]);
                }
                if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) continue;
                const auto& choice = chunk["choices"][0];
                if (choice.contains("delta") && choice["delta"].is_object()) {
                    const auto& delta = choice["delta"];
                    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                        if (!content_delta("thinking", delta["reasoning_content"].get<std::string>())) return false;
                    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                        if (!content_delta("thinking", delta["reasoning"].get<std::string>())) return false;
                    }
                    if (delta.contains("content") && delta["content"].is_string()) {
                        if (!content_delta("text", delta["content"].get<std::string>())) return false;
                    }
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& tool : delta["tool_calls"]) {
                            const int source_index = tool.value("index", 0);
                            if (source_index < 0) throw std::invalid_argument("negative tool index");
                            const auto fn = tool.value("function", json::object());
                            auto it = tools.find(source_index);
                            if (it == tools.end()) {
                                const auto name = fn.value("name", std::string());
                                if (name.empty()) throw std::invalid_argument("tool stream started without function name");
                                if (!close_active()) return false;
                                const auto id = tool.value("id", std::string());
                                if (id.empty()) throw std::invalid_argument("tool stream started without call ID");
                                ToolBlock block{next_index++, id, name};
                                it = tools.emplace(source_index, block).first;
                                if (!emit("content_block_start", {{"type", "content_block_start"}, {"index", block.index}, {"content_block", {
                                    {"type", "tool_use"}, {"id", block.id}, {"name", block.name}, {"input", json::object()}
                                }}})) return false;
                            } else if ((tool.contains("id") && tool["id"] != it->second.id) ||
                                       (fn.contains("name") && fn["name"] != it->second.name)) {
                                throw std::invalid_argument("tool identity changed during stream");
                            }
                            if (fn.contains("arguments") && fn["arguments"].is_string() && !fn["arguments"].get<std::string>().empty()) {
                                if (!emit("content_block_delta", {{"type", "content_block_delta"}, {"index", it->second.index}, {"delta", {
                                    {"type", "input_json_delta"}, {"partial_json", fn["arguments"]}
                                }}})) return false;
                            }
                        }
                    }
                }
                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                    stop_reason = map_finish_reason_to_anthropic_stop_reason(choice);
                    finished = true;
                }
            } catch (const std::exception& e) { fail(e.what()); return false; }
        }
        return true;
    };
    adapter_sink.done = [&]() {
        if (!failed && (!started || !finished || !buffer.empty())) fail("Backend stream ended without a complete terminal response");
        if (failed) { client_sink.done(); return; }
        if (!close_active()) { client_sink.done(); return; }
        for (const auto& entry : tools) {
            if (!emit("content_block_stop", {{"type", "content_block_stop"}, {"index", entry.second.index}})) { client_sink.done(); return; }
        }
        if (stop_reason == "end_turn" && !tools.empty()) stop_reason = "tool_use";
        auto final_warnings = warnings;
        const auto translated_usage = anthropic_usage_from_chat(usage, final_warnings);
        json message_delta = {{"type", "message_delta"}, {"delta", {{"stop_reason", stop_reason}, {"stop_sequence", nullptr}}},
                              {"usage", translated_usage}};
        if (!final_warnings.empty()) message_delta["warnings"] = final_warnings;
        if (emit("message_delta", message_delta)) emit("message_stop", {{"type", "message_stop"}});
        client_sink.done();
    };
    auto request = json::parse(openai_body);
    request["stream_options"]["include_usage"] = true;
    call_router(request.dump(), adapter_sink);
}

void OllamaApi::handle_anthropic_messages(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        std::vector<std::string> warnings;

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"type":"error","error":{"type":"invalid_request_error","message":"model is required"}})", "application/json");
            return;
        }

        if (model_manager_->model_exists(model)) {
            const auto info = model_manager_->get_model_info(model);
            if (info.route_policy && info.route_policy->arc_router) {
                try {
                    arc_require(info.route_policy->arc_session.has_value(),
                                "Native Messages requires ARC session configuration");
                    const auto& config = *info.route_policy->arc_router;
                    bool has_native_candidate = false;
                    for (const auto& action : config.at("actions")) {
                        const auto candidate = model_manager_->get_model_info(action.at("model").get<std::string>());
                        const auto registry = model_manager_->cloud_registry();
                        if ((candidate.recipe == "cloud" && registry &&
                             registry->wire_format_for(candidate.cloud_provider) == "anthropic") ||
                            (!info.route_policy->arc_session->codec_sha256.empty() &&
                             (candidate.recipe == "llamacpp" || (candidate.recipe == "cloud" && registry &&
                              registry->wire_format_for(candidate.cloud_provider) == "openai")))) has_native_candidate = true;
                    }
                    arc_require(has_native_candidate, "ARC collection has no native Anthropic Messages candidates");
                    const auto identity = arc_messages_identity([&req](const char* name) -> std::optional<std::string> {
                        if (!req.has_header(name)) return std::nullopt;
                        return req.get_header_value(name);
                    }, request_json);
                    auto prepared = prepare_arc_session(config, *info.route_policy->arc_session,
                        identity, request_json, "anthropic_messages", req.is_connection_closed);
                    const auto action = prepared->receipt().at("action_id").get<std::string>();
                    const auto selected = config.at("actions").at(action).at("model").get<std::string>();
                    if (prepared->receipt().at("request_format") == "openai_chat") {
                        const auto candidate = model_manager_->get_model_info(selected);
                        const auto registry = model_manager_->cloud_registry();
                        arc_require((candidate.recipe == "llamacpp" && prepared->request().at("model") == selected) || (candidate.recipe == "cloud" && registry &&
                            registry->wire_format_for(candidate.cloud_provider) == "openai" &&
                            candidate.checkpoint() == prepared->request().at("model")),
                            "ARC prepared Chat requires a matching registered Chat provider or llama.cpp model");
                        auto_load_model(selected, extract_auto_load_options(prepared->request()));
                        json routing_request = prepared->request();
                        routing_request["model"] = selected;
                        res.set_header("x-lemonade-route", "arc_session");
                        forward_arc_chat(router_, std::move(routing_request), prepared, res, req.is_connection_closed);
                        return;
                    }
                    auto match = resolve_anthropic_upstream(model_manager_, selected, prepared->request(), req, true);
                    arc_require(match.claimed, "ARC native Messages requires a registered Anthropic-format provider");
                    if (!match.upstream) {
                        set_anthropic_error_response(res, match.error_status, match.error_message);
                        return;
                    }
                    res.set_header("x-lemonade-route", "arc_session");
                    forward_anthropic_upstream(std::move(*match.upstream), request_json.value("stream", false),
                        selected, res, prepared, req.is_connection_closed);
                } catch (const std::exception& e) {
                    set_anthropic_error_response(res, 502, e.what());
                }
                return;
            }
        }

        if (req.has_param("beta")) {
            std::string beta_value = req.get_param_value("beta");
            if (beta_value != "true") {
                add_warning(warnings, "Ignored unsupported beta query value: " + beta_value);
            }
        }

        auto emit_warning_header = [&res, &warnings]() {
            if (warnings.empty()) return;
            std::ostringstream warning_header;
            for (size_t i = 0; i < warnings.size(); ++i) {
                if (i > 0) warning_header << " | ";
                warning_header << warnings[i];
            }
            res.set_header("X-Lemonade-Warning", warning_header.str());
            std::cerr << "[OllamaApi] Anthropic compatibility warnings: "
                      << warning_header.str() << std::endl;
        };

        // Relaying verbatim keeps thinking blocks, tool use, and cache control
        // intact. No router slot is taken — there is no local resource to hold.
        auto match = resolve_anthropic_upstream(model_manager_, model, request_json, req);
        if (match.claimed) {
            emit_warning_header();
            if (!match.upstream) {
                set_anthropic_error_response(res, match.error_status,
                                             match.error_message);
                return;
            }
            forward_anthropic_upstream(std::move(*match.upstream),
                                       request_json.value("stream", false), model, res);
            return;
        }

        json openai_req;
        try {
            openai_req = anthropic::convert_anthropic_to_openai_chat(request_json, warnings);
        } catch (const std::invalid_argument& e) {
            set_anthropic_error_response(res, 400, e.what());
            return;
        }

        try {
            auto_load_model(model, extract_auto_load_options(request_json));
        } catch (const RouterResidencyConflictException& e) {
            set_anthropic_residency_conflict_response(e, res);
            return;
        } catch (const std::exception&) {
            res.status = 404;
            json error = {
                {"type", "error"},
                {"error", {
                    {"type", "not_found_error"},
                    {"message", "model '" + model + "' not found, try pulling it first"}
                }}
            };
            res.set_content(error.dump(), "application/json");
            return;
        }

        bool stream = openai_req.value("stream", false);
        emit_warning_header();

        if (stream) {
            openai_req["stream"] = true;
            std::string openai_body = openai_req.dump();

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [this, openai_body, model, warnings](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;

                    anthropic::stream_openai_sse_to_anthropic_sse(openai_body, sink, model, warnings,
                        [this](const std::string& body, httplib::DataSink& s) {
                            router_->chat_completion_stream(body, s);
                        }
                    );

                    return false;
                }
            );
            return;
        }

        openai_req["stream"] = false;
        auto openai_response = router_->chat_completion(openai_req);
        if (set_anthropic_backend_error_response(openai_response, res)) {
            return;
        }

        auto anthropic_response = anthropic::convert_openai_chat_to_anthropic(openai_response, model, warnings);
        res.set_content(anthropic_response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /v1/messages: " << e.what() << std::endl;
        res.status = 500;
        json error = {
            {"type", "error"},
            {"error", {
                {"type", "api_error"},
                {"message", std::string(e.what())}
            }}
        };
        res.set_content(error.dump(), "application/json");
    }
}

}  // namespace lemon
