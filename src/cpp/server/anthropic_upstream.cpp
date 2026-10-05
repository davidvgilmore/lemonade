#include "lemon/anthropic_error.h"
#include "lemon/anthropic_relay_headers.h"
#include "lemon/anthropic_upstream.h"
#include "lemon/arc_chat_frames.h"
#include "lemon/arc_messages.h"
#include "lemon/arc_native_chat.h"
#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/utils/session_utils.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace lemon {

static bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    const auto frame = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(frame.data(), frame.size());
}

static void set_anthropic_error_response(httplib::Response& res, int status,
                                         const std::string& message) {
    res.status = status;
    res.set_content(
        anthropic::build_anthropic_error(json{{"message", message}}, status).dump(),
        "application/json");
}

using anthropic::is_forwardable_request_header;
using anthropic::is_forwardable_response_header;

AnthropicUpstreamMatch resolve_anthropic_upstream(ModelManager* model_manager,
                                                 const std::string& model,
                                                 const json& request_json,
                                                         const httplib::Request& req,
                                                         bool prepared) {
    AnthropicUpstreamMatch match;
    if (model_manager == nullptr) return match;
    CloudProviderRegistry* registry = model_manager->cloud_registry();
    if (registry == nullptr) return match;

    ModelInfo info;
    try {
        if (!model_manager->model_exists(model)) return match;
        info = model_manager->get_model_info(model);
    } catch (const std::exception&) {
        return match;
    }
    if (info.recipe != "cloud" || info.cloud_provider.empty()) return match;
    if (registry->wire_format_for(info.cloud_provider) != "anthropic") return match;

    match.claimed = true;
    auto fail = [&match](int status, std::string message) {
        match.error_status = status;
        match.error_message = std::move(message);
        return match;
    };

    const std::string base_url = registry->base_url_for(info.cloud_provider);
    if (base_url.empty()) {
        return fail(500, "provider '" + info.cloud_provider +
                         "' has no base URL configured");
    }
    const std::string api_key = registry->resolve_key(info.cloud_provider);
    if (api_key.empty()) {
        return fail(401, "no API key for provider '" + info.cloud_provider + "'; set " +
                         CloudProviderRegistry::env_var_name(info.cloud_provider) +
                         " or POST /v1/cloud/auth");
    }
    const bool allow_insecure_http =
        registry->allow_insecure_http_for(info.cloud_provider);
    if (CloudProviderRegistry::is_http_base_url(base_url) && !allow_insecure_http) {
        return fail(400, "provider '" + info.cloud_provider +
                         "' uses an http:// base URL; re-install it with "
                         "--allow-insecure-http to send the API key in plaintext");
    }

    // CloudServer::load() rejects this for the OpenAI-shaped endpoints; without
    // the same check here a hand-authored registry entry would relay an empty
    // model id and surface an opaque provider 400 instead.
    if (info.checkpoint().empty()) {
        return fail(500, "cloud model '" + model + "' is missing the 'checkpoint' "
                         "field (provider's upstream model id)");
    }

    // The body passes through byte-for-byte apart from "model", which must name
    // the provider's own id rather than lemonade's "<provider>.<id>" public one.
    json forwarded = request_json;
    if (prepared) {
        if (forwarded.at("model") != info.checkpoint()) return fail(502, "ARC prepared model does not match registered Anthropic upstream");
    } else forwarded["model"] = info.checkpoint();

    const auto auth_header = registry->auth_header_for(info.cloud_provider);
    AnthropicUpstream upstream;
    upstream.url = backends::CloudServer::upstream_url(base_url, "/messages");
    // The Anthropic API reads `beta` from the query string, so it has to be
    // reattached to the upstream URL rather than folded into a header.
    if (req.has_param("beta") && req.get_param_value("beta") == "true") {
        upstream.url += "?beta=true";
    }
    upstream.headers = backends::CloudServer::upstream_headers(auth_header, api_key,
                                                               "anthropic");
    upstream.headers["Content-Type"] = "application/json";
    // The first client value replaces whatever default upstream_headers() set;
    // a repeat of the same name is joined, since a client may send
    // anthropic-beta as several headers rather than one comma-separated value.
    std::set<std::string> from_client;
    for (const auto& [name, value] : req.headers) {
        std::string lower = name;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        if (!is_forwardable_request_header(lower)) continue;
        if (from_client.insert(lower).second) {
            upstream.headers[lower] = value;
        } else {
            upstream.headers[lower] += "," + value;
        }
    }
    session::apply_forwardable_session(upstream.headers);
    upstream.policy = backends::CloudServer::discovery_policy(base_url, allow_insecure_http);
    upstream.body = forwarded.dump();
    match.upstream = std::move(upstream);
    return match;
}

static void relay_response_headers(const std::map<std::string, std::string>& upstream,
                                   httplib::Response& res) {
    for (const auto& [name, value] : upstream) {
        if (is_forwardable_response_header(name)) {
            res.set_header(name, value);
        }
    }
}

void forward_anthropic_upstream(AnthropicUpstream upstream,
                                       bool stream,
                                       const std::string& model,
                                       httplib::Response& res,
                                       std::shared_ptr<ArcPreparedSession> prepared,
                                       std::function<bool()> cancelled) {
    if (!stream) {
        try {
            utils::RequestCancelToken cancellation;
            cancellation.should_cancel = cancelled;
            auto response = utils::HttpClient::post(
                upstream.url, upstream.body, upstream.headers, 0, upstream.policy, cancellation);
            res.status = response.status_code;
            relay_response_headers(response.headers, res);
            if (prepared) {
                const auto parsed = json::parse(response.body, nullptr, false);
                const auto assistant = parsed.is_discarded() ? json(nullptr) : arc_messages_assistant(parsed);
                const bool terminal = response.status_code == 200 && !assistant.is_null();
                auto body = std::make_shared<std::string>(std::move(response.body));
                res.set_content_provider(body->size(), "application/json",
                    [body](size_t offset, size_t length, httplib::DataSink& sink) {
                        return sink.write(body->data() + offset, length);
                    },
                    [prepared, assistant, terminal](bool success) { prepared->finish(success && terminal, assistant); });
            } else res.set_content(response.body, "application/json");
        } catch (const std::exception& e) {
            set_anthropic_error_response(
                res, 502, "cloud request for '" + model + "' failed: " + e.what());
        }
        return;
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    // Everything below runs inside the content provider, after the status line
    // is already on the wire. That costs the real status on a non-200 (relayed
    // as an SSE error frame instead) but keeps every blocking wait somewhere
    // sink.is_writable can observe the peer, so a client that disappears cannot
    // strand the upstream transfer.
    auto observed = std::make_shared<ArcMessagesStream>();
    auto accepted_terminal = std::make_shared<bool>(false);
    res.set_chunked_content_provider(
        "text/event-stream",
        [upstream = std::move(upstream), model, prepared, observed, accepted_terminal, cancelled](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) return false;
            // Both ends speak Anthropic SSE, so a 200 relays unparsed. A
            // non-200 body is not SSE, so it is diverted and re-emitted as an
            // error event rather than written into the event stream.
            static constexpr size_t max_error_body = 64 * 1024;
            int upstream_status = 200;
            std::string error_body;
            std::map<std::string, std::string> response_headers;
            auto emit_error = [&sink, &model](int status, const std::string& message) {
                write_sse_event(sink, "error", anthropic::build_anthropic_error(
                    json{{"message", "cloud request for '" + model + "' " + message}},
                    status));
            };

            try {
                auto result = utils::HttpClient::post_stream(
                    upstream.url, upstream.body,
                    [&](const char* data, size_t length) {
                        if (upstream_status != 200) {
                            if (error_body.size() < max_error_body) {
                                error_body.append(
                                    data,
                                    std::min(length, max_error_body - error_body.size()));
                            }
                            return true;
                        }
                        if (prepared && !observed->accept(data, length)) return false;
                        const bool written = sink.write(data, length);
                        *accepted_terminal = *accepted_terminal || (written && prepared && observed->terminal());
                        return written && !*accepted_terminal;
                    },
                    upstream.headers, 0,
                    [&upstream_status](int status) { upstream_status = status; },
                    upstream.policy,
                    [&sink, cancelled]() { return (cancelled && cancelled()) || (sink.is_writable && !sink.is_writable()); },
                    &response_headers);
                // on_status never fires when the body is empty, so fall back to
                // the code curl always records after the transfer.
                const int status =
                    upstream_status != 200 ? upstream_status : result.status_code;
                if (status != 200) {
                    emit_error(status, "failed with status " + std::to_string(status) +
                                       (error_body.empty() ? "" : ": " + error_body));
                } else if (result.curl_code != 0 && !*accepted_terminal && sink.is_writable && sink.is_writable()) {
                    emit_error(502, "stream ended early: " + result.curl_error);
                }
            } catch (const std::exception& e) {
                emit_error(502, std::string("failed: ") + e.what());
            }
            sink.done();
            return prepared ? *accepted_terminal : false;
        },
        [prepared, observed, accepted_terminal](bool) {
            if (prepared) prepared->finish(*accepted_terminal, observed->messages());
        }
    );
}


void forward_arc_messages_as_chat(AnthropicUpstream upstream,
                                  const std::shared_ptr<ArcPreparedSession>& prepared,
                                  httplib::Response& res, const json& route_decision,
                                  std::function<bool()> cancelled) {
    if (!prepared->request().value("stream", false)) {
        utils::RequestCancelToken cancellation;
        cancellation.should_cancel = cancelled;
        const auto response = utils::HttpClient::post(upstream.url, upstream.body,
            upstream.headers, 0, upstream.policy, cancellation);
        arc_require(response.status_code == 200, "ARC native provider refused request");
        arc_require(response.body.size() <= 16 * 1024 * 1024, "ARC native response exceeds 16 MiB");
        const auto native = json::parse(response.body);
        arc_require(!arc_messages_assistant(native).is_null(), "ARC native provider did not return terminal success");
        auto translated = prepared->transform("response", {{"body", native}}, cancelled).at("body");
        const auto assistant = arc_chat_assistant(translated);
        arc_require(!assistant.is_null(), "ARC codec did not return a terminal Chat response");
        translated["x_lemonade_route"] = route_decision;
        auto body = std::make_shared<std::string>(translated.dump());
        arc_require(body->size() <= 16 * 1024 * 1024, "ARC translated response exceeds 16 MiB");
        relay_response_headers(response.headers, res);
        res.set_content_provider(body->size(), "application/json",
            [body](size_t offset, size_t length, httplib::DataSink& sink) {
                return sink.write(body->data() + offset, length);
            }, [prepared, assistant](bool success) { prepared->finish(success, assistant); });
        return;
    }
    const auto initial = prepared->transform("stream_start", json::object(), cancelled);
    auto delivered = std::make_shared<ArcChatCodecDelivery>();
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider("text/event-stream",
        [upstream = std::move(upstream), prepared, cancelled, initial, delivered](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) return false;
            const auto stopped = [&]() {
                return (cancelled && cancelled()) || (sink.is_writable && !sink.is_writable());
            };
            const auto write = [&](const char* data, size_t size) {
                return !stopped() && sink.write(data, size);
            };
            try {
                arc_require(delivered->deliver(initial, false, write), "ARC initial Chat frames invalid");
                ArcMessagesFrames frames;
                uint64_t sequence = 0;
                int status = 0;
                const auto result = utils::HttpClient::post_stream(upstream.url, upstream.body,
                    [&](const char* data, size_t size) {
                        if (status != 200 || stopped()) return false;
                        const bool valid = frames.accept(data, size, [&](const std::string& frame, bool terminal) {
                            auto output = prepared->transform("stream_push", {{"sequence", sequence++}, {"frame", frame}}, stopped);
                            arc_require(output.contains("frames"), "ARC codec omitted Chat frames");
                            if (!delivered->deliver(output, false, write)) return false;
                            if (terminal) {
                                output = prepared->transform("stream_finish", {{"sequence", sequence++}}, stopped);
                                arc_require(output.contains("frames"), "ARC codec omitted final Chat frames");
                                if (!delivered->deliver(output, true, write)) return false;
                                arc_require(delivered->accepted_terminal(), "ARC codec did not produce Chat terminal");
                            }
                            return true;
                        });
                        return valid && !delivered->accepted_terminal();
                    }, upstream.headers, 0, [&status](int value) { status = value; },
                    upstream.policy, stopped);
                if (!delivered->accepted_terminal()) {
                    // Never fabricate [DONE] or a successful settlement on native/codec failure.
                    const auto error = "data: " + json({{"error", {{"message", "ARC native provider stream incomplete"},
                        {"type", "arc_routing_error"}, {"upstream_status", result.status_code}}}}).dump() + "\n\n";
                    write(error.data(), error.size());
                }
            } catch (const std::exception&) {
                // A failed translation leaves no successful terminal; releaser aborts.
            }
            sink.done();
            return delivered->accepted_terminal();
        }, [prepared, delivered](bool) {
            prepared->finish(delivered->accepted_terminal(), delivered->messages());
        });
}

} // namespace lemon
