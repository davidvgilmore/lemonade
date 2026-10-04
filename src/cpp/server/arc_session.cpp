#include "lemon/arc_session.h"
#include "lemon/utils/http_client.h"
#include <lemon/utils/aixlog.hpp>

namespace lemon {

static json settle_arc_session(const std::string& endpoint, const json& request) {
    try {
        auto response = utils::HttpClient::post(endpoint, request.dump(),
            {{"Content-Type", "application/json"}}, 10,
            utils::HttpSecurityPolicy::TrustedLoopback);
        arc_require(response.status_code == 200, "ARC session settlement failed");
        auto result = json::parse(response.body);
        const bool commit = endpoint.size() >= 7 && endpoint.compare(endpoint.size() - 7, 7, "/commit") == 0;
        arc_require(result.value("state", "") == (commit ? "committed" : "aborted"),
                    "ARC session settlement was not acknowledged");
        return result;
    } catch (...) {
        LOG(ERROR, "ARC") << "Session settlement acknowledgment unavailable; outcome unknown" << std::endl;
        throw;
    }
}

std::shared_ptr<ArcPreparedSession> prepare_arc_session(
    const json& config, const ArcSessionConfig& session, const ArcSessionIdentity& identity,
    const json& request, const std::string& format, std::function<bool()> cancelled) {
    const auto payload = arc_session_prepare_payload(config, session, identity, request, format);
    const auto gate_key = json::array({session.endpoint, session.owner_id, identity.session_id}).dump();
    auto lease = std::make_shared<ArcSessionLease>(gate_key, cancelled);
    utils::RequestCancelToken cancellation;
    cancellation.should_cancel = std::move(cancelled);
    auto response = utils::HttpClient::post(session.endpoint + "/prepare", payload.dump(),
        {{"Content-Type", "application/json"}}, 120,
        utils::HttpSecurityPolicy::TrustedLoopback, cancellation);
    arc_require(response.status_code == 200, "ARC session service refused preparation");
    auto receipt = json::parse(response.body);
    auto prepared = std::make_shared<ArcPreparedSession>(session, receipt, settle_arc_session, std::move(lease));
    validate_arc_session_receipt(config, session, receipt, request, format);
    return prepared;
}

} // namespace lemon
