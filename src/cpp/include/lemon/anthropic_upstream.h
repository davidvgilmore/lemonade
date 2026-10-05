#pragma once

#include <httplib.h>

#include "lemon/arc_session.h"
#include "lemon/model_manager.h"
#include "lemon/utils/http_client.h"

namespace lemon {

struct AnthropicUpstream {
    std::string url;
    std::map<std::string, std::string> headers;
    utils::HttpSecurityPolicy policy = utils::HttpSecurityPolicy::ExternalHttpsOnly;
    std::string body;
};

// `claimed` separates "this model isn't relayed, fall through to conversion"
// from "it is relayed but the provider is unusable" — the latter must report
// its own error instead of failing further down as a misleading local-model
// load failure.
struct AnthropicUpstreamMatch {
    bool claimed = false;
    std::optional<AnthropicUpstream> upstream;
    int error_status = 0;
    std::string error_message;
};

AnthropicUpstreamMatch resolve_anthropic_upstream(ModelManager* model_manager,
    const std::string& model, const json& request_json, const httplib::Request& req,
    bool prepared = false);
void forward_anthropic_upstream(AnthropicUpstream upstream, bool stream,
    const std::string& model, httplib::Response& res,
    std::shared_ptr<ArcPreparedSession> prepared = nullptr,
    std::function<bool()> cancelled = {});
void forward_arc_messages_as_chat(AnthropicUpstream upstream,
    const std::shared_ptr<ArcPreparedSession>& prepared, httplib::Response& res,
    const json& route_decision, std::function<bool()> cancelled = {});

} // namespace lemon
