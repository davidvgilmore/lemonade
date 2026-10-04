#pragma once

#include <functional>
#include <string>
#include <vector>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace lemon::anthropic {
using json = nlohmann::json;
using StreamFn = std::function<void(const std::string&, httplib::DataSink&)>;
json convert_anthropic_to_openai_chat(const json& request, std::vector<std::string>& warnings);
json convert_openai_chat_to_anthropic(const json& response, const std::string& model, const std::vector<std::string>& warnings);
void stream_openai_sse_to_anthropic_sse(const std::string& body, httplib::DataSink& sink,
                                      const std::string& model, const std::vector<std::string>& warnings,
                                      StreamFn call_router);
} // namespace lemon::anthropic
