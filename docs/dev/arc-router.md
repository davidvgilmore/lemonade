# ARC policy worker integration (experimental)

ARC is a decision policy, not a text generator. The `collection.router` ARC
adapter calls a separately launched local worker that owns encoder and head
inference. It uses the existing
`POST /v1/rayline/arc/policy/decide` contract. No ARC implementation, credentials,
weights, or release-specific configuration are bundled in Lemonade.

## Design choice

Three extension shapes were considered:

- A `WrappedServer` backend would fit subprocess lifecycle and model management,
  but its generation interface does not represent a structured policy decision.
  A backend implementation alone cannot install a routing policy.
- A classifier could reuse first-match rules, but the current classifier input
  is the latest user text. ARC needs message/tool history, attribution, eligible
  actions, session context, and package identity. Turning a hierarchical action
  into classifier scores would discard that contract.
- A typed `routing.router` branch preserves the existing collection registration,
  candidate resolution, route trace, and dispatch paths. The adapter remains
  independent of the worker's tensor implementation. This is the implemented
  option; it is a compiled Lemonade extension, not a stock runtime plugin.

The worker remains an external process, consistent with Lemonade's subprocess
inference invariant. This first integration does not install or supervise it.
Only numeric IPv4 loopback is accepted; HTTP redirects are disabled. Requests
use a bounded timeout and inherit chat request cancellation.

## Policy configuration

Use an ordinary `collection.router` collection with `routing.candidates` and
`routing.default_model`, then an ARC router instead of rules/classifiers:

```json
{
  "type": "arc",
  "endpoint": "http://127.0.0.1:18081/v1/rayline/arc/policy/decide",
  "package": {
    "alias": "operator-configured-package",
    "package_sha256": "<exact 64-character manifest SHA256>"
  },
  "actions": {
    "<opaque 64-character action ID>": {
      "model": "registered-candidate",
      "reasoning_effort": "high",
      "reasoning_max_tokens": null,
      "steering_suffix": ""
    }
  }
}
```

The placeholders above must be replaced from the pinned package catalog.
Each eligible action needs a destination binding. Several actions can map to
one destination with different native reasoning efforts. The binding is operator
configuration; the worker response cannot supply arbitrary request overrides.

A wrong package, wrong schema, unavailable worker, or action outside the request's
eligible set fails closed. ARC never falls through to `default_model` after an
inference failure. The default remains required by the collection schema.

## Decision-only validation

Submit `{ "policy": <collection>, "arc_request": <complete decision request> }`
to `/api/v1/routing/validate`. The full existing ARC envelope is forwarded without
rewriting its history, format, attribution, selection, or session fields. The
response's `decision.outputs.arc` contains the worker response unchanged,
including encoding diagnostics, action scores, and selection reason. This path
does not register the collection, load a target model, or generate an answer.
The other standard endpoint prefixes also work.

This is the preferred numerical-parity acceptance path: supply the same pinned
package, input, selection, session trajectory, and attribution used by the
reference service; compare selected action/arm and encoding/score diagnostics
under the release's stated tolerances. A successful adapter round trip alone
does not establish numerical parity or local model execution. Establish those
separately from the actual worker's runtime/artifact evidence.

## Chat dispatch

Register the collection through `/api/v1/pull` and address its name in a chat
completion. Include `arc_context`, containing the normal decision-request
envelope **except `request`**. It must explicitly provide schema, pinned package,
`request_format: "openai_chat"`, episode hash, context epoch, attribution, and
selection. The adapter builds the worker's `request` from the original
`system`, `tools`, and `messages` fields. It never substitutes another message
history or invents attribution. Generation controls remain on the original
request; they are not part of the worker's input schema.

After selection, Lemonade applies the configured `reasoning_effort`, strips
`arc_context`, and dispatches normally. With `route_trace: true`, the raw worker
result appears in `x_lemonade_route.outputs.arc`. Native controls are not
inferred from a model name.

The current dispatch adapter supports OpenAI chat and native `reasoning_effort`.
Nonempty steering suffixes and reasoning budgets are accepted as explicit
catalog metadata for decision-only validation. Chat dispatch rejects requests
with those eligible actions before calling the worker: their provider-specific
encoding and stateful steering ledger must be implemented and tested before
those actions can be dispatched. Conflicting incoming `thinking`, `reasoning`,
`reasoning_max_tokens`, and `chat_template_kwargs` are rejected. Responses,
Anthropic, and completions dispatch are not implemented; decision-only
validation can forward any format accepted by the worker.

## Verification

```sh
cmake -S . -B build -G Ninja -DBUILD_WEB_APP=OFF -DBUILD_TESTING=ON
cmake --build build --target lemond test_arc_router test_routing_policy_parser test_routing_policy_llm_router
ctest --test-dir build -R 'ArcRouterTest|RoutingPolicyParserTest|RoutingPolicyLlmRouterTest' --output-on-failure
# Start an isolated lemond before this command; follow test/requirements.txt.
LEMONADE_TEST_PORT=18305 python test/server_arc.py
```

The Python test uses the repository's server test base, a synthetic worker, and a
mock provider. It checks the real HTTP validation path, collection registration,
model/effort delivery, unchanged messages, and fail-closed rejection. No private
artifacts or paid provider requests are needed. Private release parity fixtures
and results belong outside this public repository.

## Desktop and CLI setup

In the desktop app, create a **Hybrid Router**, choose **ARC**, and import the
collection setup exported by your ARC runtime. Select the corresponding
candidate models, then save. Existing ARC collections open in ARC mode; editing
and exporting retains the complete action bindings, including controls that are
only supported for decision testing. A changed candidate selection must still
contain every bound destination.

The **Test Prompt** tab accepts a saved ARC decision request and tests it against
the local runtime. The result is a selected destination, without a downstream
model call. This checks connectivity and the supplied conversation; it does not
qualify a model release against reference outputs. Conversation files can
contain private inputs, so keep them outside source control.

The existing CLI uses the same collection format:

```sh
lemonade import arc-router.json
lemonade export user.MyArcRouter --output arc-router-export.json
```

Use `lemonade --help` for the server connection options on your installation.
Import registers a router; it does not install the separate ARC runtime or
establish numerical parity. Start that runtime with access to the pinned release
before testing. For a private Hugging Face release, give the runtime its existing
Hugging Face credentials (`HF_TOKEN` or its supported local login). Never put
credentials in the collection JSON. Lemonade's own model download credentials
are separate from the ARC runtime's process environment.

The router appears in the normal model picker. Desktop chat currently cannot
supply the required ARC conversation context by itself. API clients must supply
`arc_context`; action sets requiring steering or budgets are rejected for chat.
The local decision test supports those catalog controls without dispatching
anything. Full ordinary-chat support requires the shared runtime's session and
steering lifecycle integration and remains unfinished.

Desktop chat and `lemonade chat` supply the existing `X-Client-Session-Id`
header and a fresh `X-Lemonade-Request-Id` for each request attempt. The session
survives turns, while New Chat and CLI history resets start a new session.
Independent windows or REPL processes receive independent identities. These
headers identify requests; they do not attest completion, supply model
attribution, or commit routing state. The experimental session adapter remains
outside the public API until its streaming and retry lifecycle is qualified.

Run `python test/cli_chat_identity.py` after building the CLI to check both
streamed and non-streamed requests against a synthetic local HTTP server.
