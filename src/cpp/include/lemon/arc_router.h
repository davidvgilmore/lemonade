#pragma once

#include "lemon/routing_policy.h"

#include <algorithm>
#include <functional>
#include <regex>
#include <set>
#include <stdexcept>

namespace lemon {

class ArcRoutingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void arc_require(bool ok, const std::string& reason) {
    if (!ok) throw ArcRoutingError(reason);
}

inline bool arc_sha256(const json& value) {
    return value.is_string() && std::regex_match(value.get<std::string>(), std::regex("[0-9a-f]{64}"));
}

inline void validate_arc_router(const json& config) {
    const std::set<std::string> keys = {"type", "endpoint", "package", "actions", "session"};
    for (const auto& item : config.items()) {
        arc_require(keys.count(item.key()) != 0, "Unknown ARC router config key");
    }
    arc_require(config.at("type") == "arc", "Expected ARC router type");
    if (!config.contains("session")) arc_require(std::regex_match(config.at("endpoint").get<std::string>(),
        std::regex("http://127\\.0\\.0\\.1:[0-9]{1,5}/v1/rayline/arc/policy/decide")),
        "ARC endpoint must be the policy decision endpoint on numeric loopback");
    const auto& package = config.at("package");
    arc_require(package.is_object() && package.size() == 2 &&
        package.at("alias").is_string() && !package.at("alias").get<std::string>().empty() &&
        arc_sha256(package.at("package_sha256")), "ARC package requires alias and exact SHA256");
    arc_require(config.at("actions").is_object() && !config.at("actions").empty(), "ARC actions are required");
    for (const auto& [id, binding] : config.at("actions").items()) {
        arc_require(arc_sha256(id), "ARC action identity must be SHA256");
        arc_require(binding.is_object() && binding.size() == (config.contains("session") ? 5 : 4) &&
            binding.at("model").is_string() && !binding.at("model").get<std::string>().empty(),
            "ARC binding requires model, reasoning_effort, reasoning_max_tokens, steering_suffix");
        if (config.contains("session")) {
            arc_require(binding.at("wire_model").is_string() && !binding.at("wire_model").get<std::string>().empty(),
                        "ARC session actions require explicit wire_model");
        }
        arc_require(binding.at("reasoning_effort").is_null() || binding.at("reasoning_effort").is_string(),
            "ARC reasoning_effort must be explicit string or null");
        arc_require(binding.at("steering_suffix").is_string(), "ARC steering_suffix must be explicit text");
        const auto& budget = binding.at("reasoning_max_tokens");
        arc_require(budget.is_null() || (budget.is_number_integer() && budget.get<int64_t>() >= 0),
            "ARC reasoning_max_tokens must be null or a nonnegative integer");
    }
}

inline json arc_request_from_chat(const json& body) {
    arc_require(body.contains("arc_context") && body.at("arc_context").is_object(), "Explicit arc_context required");
    json request = body.at("arc_context");
    arc_require(!request.contains("request"), "arc_context must not replace the incoming request");
    arc_require(request.at("request_format") == "openai_chat" && body.contains("messages"),
        "ARC dispatch currently requires OpenAI chat messages");
    request["request"] = json::object();
    for (const auto* key : {"system", "tools", "messages"}) {
        if (body.contains(key)) request["request"][key] = body.at(key);
    }
    return request;
}

inline void validate_arc_chat_bindings(const json& config, const json& request) {
    for (const auto& action : request.at("selection").at("available_action_ids")) {
        const auto& binding = config.at("actions").at(action.get<std::string>());
        arc_require(binding.at("reasoning_max_tokens").is_null() && binding.at("steering_suffix") == "",
            "ARC chat dispatch does not implement reasoning budgets or stateful steering; use decision-only validation");
    }
}

inline Decision route_arc(const json& config, const json& request,
                          const std::function<json(const std::string&, const json&)>& call) {
    try {
        arc_require(request.at("schema_version") == "rayline.arc.policy-decision-request.v1", "ARC request schema mismatch");
        arc_require(request.at("package") == config.at("package"), "ARC request package mismatch");
        arc_require(request.contains("episode_id_hash") && request.contains("context_epoch") &&
            request.contains("attribution") && request.contains("request"), "Explicit ARC session and attribution required");
        const auto& eligible = request.at("selection").at("available_action_ids");
        arc_require(eligible.is_array() && !eligible.empty(), "ARC eligible actions required");
        for (const auto& id : eligible) {
            arc_require(id.is_string() && config.at("actions").contains(id.get<std::string>()), "ARC action has no destination binding");
        }
        const json response = call(config.at("endpoint").get<std::string>(), request);
        arc_require(response.at("schema_version") == "rayline.arc.policy-decision-response.v1", "ARC response schema mismatch");
        arc_require(response.at("package") == config.at("package"), "ARC response package mismatch");
        const std::string selected = response.at("decision").at("selected_action_id").get<std::string>();
        arc_require(std::find(eligible.begin(), eligible.end(), selected) != eligible.end(), "ARC selected an ineligible action");
        const auto& binding = config.at("actions").at(selected);
        Decision decision;
        decision.route_to = binding.at("model").get<std::string>();
        decision.matched_rule = "arc";
        decision.outputs["arc"] = response;
        decision.request_overrides["reasoning_effort"] = binding.at("reasoning_effort");
        return decision;
    } catch (const ArcRoutingError&) {
        throw;
    } catch (const std::exception&) {
        throw ArcRoutingError("ARC decision failed or returned an invalid contract");
    }
}

inline void apply_arc_dispatch(json& request, const Decision& decision) {
    if (decision.matched_rule != "arc") return;
    for (const auto* key : {"reasoning", "thinking", "reasoning_max_tokens", "chat_template_kwargs"}) {
        arc_require(!request.contains(key), "Conflicting native reasoning control in ARC request");
    }
    for (const auto& [key, value] : decision.request_overrides.items()) {
        if (value.is_null()) request.erase(key);
        else request[key] = value;
    }
    request.erase("arc_context");
}

} // namespace lemon
