#include "lemon/arc_router.h"
#include "lemon/routing_policy_parser.h"

#include <cstdio>
#include <stdexcept>

using lemon::json;

static void check(bool ok, const char* name) {
    if (!ok) throw std::runtime_error(name);
    std::printf("PASS %s\n", name);
}

template<class F> static void rejects(F fn, const char* name) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected, name);
}

int main() {
    const std::string action(64, 'a');
    const json package = {{"alias", "synthetic"}, {"package_sha256", std::string(64, 'b')}};
    const json config = {
        {"type", "arc"}, {"endpoint", "http://127.0.0.1:18081/v1/rayline/arc/policy/decide"},
        {"package", package},
        {"actions", {{action, {{"model", "candidate"}, {"reasoning_effort", "high"},
                               {"reasoning_max_tokens", nullptr}, {"steering_suffix", ""}}}}}
    };
    const json policy_doc = {
        {"version", "1"}, {"recipe", "collection.router"}, {"components", {"candidate"}},
        {"routing", {{"candidates", {"candidate"}}, {"default_model", "candidate"}, {"router", config}}}
    };
    auto policy = lemon::parse_route_policy_collection(policy_doc, {});
    check(policy.arc_router.has_value() && policy.helper_models.empty(), "typed ARC policy does not invoke a classifier model");
    const json context = {
        {"schema_version", "rayline.arc.policy-decision-request.v1"}, {"package", package},
        {"episode_id_hash", std::string(64, 'c')}, {"context_epoch", "epoch-1"},
        {"attribution", json::array()}, {"request_format", "openai_chat"},
        {"selection", {{"available_action_ids", {action}}}}
    };
    const json body = {{"model", "user.arc"}, {"arc_context", context}, {"temperature", 0.2},
        {"messages", {{{"role", "system"}, {"content", "preserve\nbytes"}},
                       {{"role", "user"}, {"content", "synthetic request"}}}}};
    auto request = lemon::arc_request_from_chat(body);
    check(request.at("request").at("messages") == body.at("messages") &&
          !request.at("request").contains("temperature"), "projection preserves messages and excludes generation controls");
    const json response = {{"schema_version", "rayline.arc.policy-decision-response.v1"},
        {"package", package}, {"decision", {{"selected_action_id", action}}},
        {"encoding", {{"representation_id", "synthetic"}}}};
    int calls = 0;
    auto service = [&](const std::string& endpoint, const json& sent) {
        ++calls;
        check(endpoint == config.at("endpoint") && sent == request, "service receives exact contract");
        return response;
    };
    const auto decision = lemon::route_arc(config, request, service);
    check(calls == 1 && decision.route_to == "candidate" && !decision.default_used &&
          decision.outputs.at("arc") == response, "selected action and raw diagnostics retained without recomputation");
    json dispatch = body;
    lemon::apply_arc_dispatch(dispatch, decision);
    check(dispatch.at("reasoning_effort") == "high" && !dispatch.contains("arc_context") &&
          dispatch.at("messages") == body.at("messages") && dispatch.at("temperature") == 0.2,
          "native effort applied and original request preserved");
    auto invalid = response;
    invalid["package"]["package_sha256"] = std::string(64, 'd');
    rejects([&] { lemon::route_arc(config, request, [&](auto&, auto&) { return invalid; }); }, "wrong package fails closed");
    invalid = response;
    invalid["decision"]["selected_action_id"] = std::string(64, 'd');
    rejects([&] { lemon::route_arc(config, request, [&](auto&, auto&) { return invalid; }); }, "ineligible action fails closed");
    invalid = response;
    invalid["schema_version"] = "other";
    rejects([&] { lemon::route_arc(config, request, [&](auto&, auto&) { return invalid; }); }, "wrong schema fails closed");
    rejects([&] { lemon::route_arc(config, request, [](auto&, auto&) -> json { throw std::runtime_error("offline"); }); }, "worker outage does not use default model");
    auto bad_config = config;
    bad_config["endpoint"] = "http://127.0.0.1.attacker.example:80/v1/rayline/arc/policy/decide";
    rejects([&] { lemon::validate_arc_router(bad_config); }, "non-loopback destination rejected");
    bad_config = config;
    bad_config["actions"][action]["steering_suffix"] = "unsupported";
    lemon::validate_arc_router(bad_config);
    rejects([&] { lemon::validate_arc_chat_bindings(bad_config, request); }, "unsupported steering fails before chat inference");
    check(lemon::route_arc(bad_config, request, [&](auto&, auto&) { return response; }).route_to == "candidate",
          "decision-only validation preserves actions with unsupported dispatch controls");
    dispatch = body;
    dispatch["thinking"] = {{"type", "enabled"}};
    rejects([&] { lemon::apply_arc_dispatch(dispatch, decision); }, "conflicting reasoning controls rejected");
    auto spoofed = body;
    spoofed["arc_context"]["request"] = {{"messages", json::array()}};
    rejects([&] { lemon::arc_request_from_chat(spoofed); }, "context cannot substitute request");
    auto no_attribution = request;
    no_attribution.erase("attribution");
    rejects([&] { lemon::route_arc(config, no_attribution, service); }, "missing attribution stays missing");
    check(calls == 1, "invalid input rejected before inference");
}
