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

## Replay Chat dispatch

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

The replay dispatch adapter supports OpenAI chat and native `reasoning_effort`.
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

The router appears in the normal model picker. Replay collections require
`arc_context`; ordinary desktop and CLI requests instead use the opt-in session
service configuration below.

Desktop chat and `lemonade chat` supply the existing `X-Client-Session-Id`
header and a fresh `X-Lemonade-Request-Id` for each request attempt. The session
survives turns, while New Chat and CLI history resets start a new session.
Independent windows or REPL processes receive independent identities. These
headers identify requests; they do not attest completion, supply model
attribution, or commit routing state. The session service owns attribution and the steering ledger; the host settles
its prepared receipt using the actual response delivery lifecycle.

Run `python test/cli_chat_identity.py` after building the CLI to check both
streamed and non-streamed requests against a synthetic local HTTP server.

## Ordinary Chat sessions

Add `session` to the ARC router configuration and `wire_model` to every action:

```json
{
  "type": "arc",
  "package": {"alias": "operator-configured-package", "package_sha256": "<64 hex characters>"},
  "session": {"endpoint": "http://127.0.0.1:18082/v1/rayline/arc/session", "owner_id": "this-lemonade-installation"},
  "actions": {
    "<64-character action ID>": {
      "model": "registered-candidate",
      "wire_model": "provider/upstream-model",
      "reasoning_effort": null,
      "reasoning_max_tokens": 128,
      "steering_suffix": "operator-configured steering text"
    }
  }
}
```

The session service is a separately managed process. `/experimental/arc/session`
is also accepted for development services implementing this same contract.
`owner_id` must be stable and distinct across installations sharing that service.
It is an identity, not a credential. Package catalog and session-service action
bindings must agree. Cloud `wire_model` must equal the registered candidate's
upstream checkpoint ID; routing still selects the registered Lemonade model.

Send normal `/v1/chat/completions` requests with `X-Client-Session-Id` and
`X-Lemonade-Request-Id`. The desktop and interactive CLI already supply both.
Keep session identity for a conversation and operation identity for a retry of
the same attempt. A new request attempt uses a new operation identity. Caller
constructed `arc_context` is rejected in session mode. One assistant choice is
supported. Missing identity fails closed before any service or provider call.

Lemonade posts the original native request, owner/operation identity,
`metadata.session_id`, `request_format: openai_chat`, and configured
`available_action_ids` to `/prepare`. The service owns projection, attribution,
held-model scheduling, native controls and private append history. It returns
`owner_id`, `package_sha256`, `action_id`, `transaction_id`, `session_token`,
`episode_id_hash`, `context_epoch`, `source_request_format`, `request_format`,
`request`, and the full policy `decision`. Lemonade validates the configured
package, eligible action, numerical arm SHA256 and nonnegative integer session
revision, native format and upstream destination. It dispatches
the prepared request through existing cloud or llama.cpp transport with normal
credentials and HTTP security checks. Later thinking normalization, tool-schema
rewrites, legacy token aliases and automatic usage requests are skipped for
that prepared request. The service must deliberately include any desired
`stream_options.include_usage`; absent usage remains unknown.

After the downstream HTTP transport accepts a successful terminal response,
Lemonade posts `/commit` with owner/session token,
`settlement: successful_2xx_terminal_sent`, and exact assistant messages. A
buffered response retains its entire native assistant object. Standard streamed
text, reasoning text and tool arguments are accumulated from delivered deltas.
Unrecognized streamed assistant fields are forwarded unchanged, but settlement
records `response_messages: null, response_attribution: unknown` rather than
inventing opaque/signed history. Streams require both finish reason and `[DONE]`;
partial streams, provider errors, failed loads and client disconnects abort.
The stream observer is bounded to 16 MiB per response. Settlement occurs outside
router locks. Requests within a session wait for the previous settlement
acknowledgment; different sessions proceed independently. Failed settlement is
logged, and the host blocks further prepares for that session. The service may
have committed even when its acknowledgment was lost. Reconcile the receipt at
the service before explicitly restarting the host to clear this in-memory block;
do not change session identity to bypass an unresolved receipt.
A session service must make repeated settlement idempotent and reject reuse of
an operation identity with a different source request.

Responses and completions remain separate, unsupported protocol paths.
Session collections fail closed on those paths. Local llama.cpp prepared transport is implemented but numerical ARC
encoder/head parity and real local-model composition remain separate checks.

Run `LEMONADE_TEST_PORT=<isolated port> python test/server_arc_session.py` against
a freshly built isolated `lemond`. The synthetic test asserts actual provider
wire equality and buffered/stream terminal commit, invalid receipt refusal,
provider failure, partial-stream and disconnected-client abort. It also delays
a commit acknowledgment to verify immediate continuation ordering and independent
session concurrency, rejects malformed numerical receipts, and blocks
continuation after a failed settlement acknowledgment. It makes no paid provider
calls.
`ArcSessionTest` is part of `cpp-ci` and checks fragmented tool history, opaque
history disposition and settlement ownership. These transport checks do not
qualify an ARC checkpoint or the session service's own policy/ledger math.

### Native Messages sessions

The same session collection and identity headers work with `/v1/messages` when
it has registered cloud candidates whose provider `wire_format` is `anthropic`.
Lemonade passes the full configured action list to the service. A collection
with no native candidate fails before preparation; if the selected winner lacks
native transport, Lemonade aborts that receipt and refuses dispatch. It does not
substitute a different winner or narrow the ARC basket to suit a protocol.
The service must retain the pinned full trained Stage-1 basket and refuse an
unavailable winner rather than renormalizing or retraining the basket. Local
Qwen requires the separately opted-in cross-format contract below; this native
relay does not perform a conversion.

The service receives the original Messages body with
`request_format: anthropic_messages` and must return that same native format.
Preparation happens before the existing Anthropic relay. The prepared model
must match the selected provider's registered checkpoint. The relay preserves
prepared system blocks, tool results, private append placement, thinking controls,
cache-control fields, Anthropic version/beta headers, and existing authentication.
Buffered responses retain native content blocks and provider usage unchanged.
Streamed text, thinking, signature and tool-input deltas reconstruct assistant
history; redacted thinking and untouched block fields remain opaque and exact.
Unknown deltas are forwarded but produce explicitly unknown attribution.

Native streaming requires a real stop reason and `message_stop`, with every
started content block stopped. It settles after that terminal event is accepted,
even if the provider leaves HTTP open or the client closes immediately afterward.
Partial streams, provider errors and earlier disconnects abort. The same
per-session acknowledgment ordering and unresolved-session quarantine apply.
These are host transport properties; cross-model signature disposition and
private ledger behavior remain the session service's responsibility.

Run `LEMONADE_TEST_PORT=<isolated port> python test/server_arc_messages.py` against
a freshly built isolated server. Its synthetic provider checks native request
wire equality, signed/redacted/tool block history, cache intent and reported usage,
unknown delta disposition, terminal-without-EOF, continuation, failure and drop.
It runs alongside the Chat fixture in the existing router CI stages. Neither
fixture establishes numerical ARC parity or qualification of a real provider.

### Messages sessions with a prepared Chat provider

A session configuration may additionally set `codec_sha256` to the SHA-256
identity of its qualified request/return codec implementation. With that opt-in,
Messages ingress can accept a prepared `openai_chat` request. The service receipt
must retain `source_request_format: anthropic_messages` and contain exactly:

```json
{
  "response_codec": {
    "schema_version": "rayline.arc.response-codec.v1",
    "source": "openai_chat",
    "target": "anthropic_messages",
    "implementation_sha256": "<configured codec SHA-256>"
  }
}
```

Preparation still receives the entire configured action basket. The service
applies private history and native controls before encoding the provider request.
It must refuse an unsupported selected winner, without selecting another action.
Lemonade validates the prepared model against the selected cloud registration's
checkpoint. For a local llama.cpp destination, the prepared model and action's
`wire_model` must equal the selected registered Lemonade alias. Other backend
recipes and wire formats are refused. The host loads that destination and uses
its existing credentials, HTTP security policy, cancellation and transport.
The prepared body is sent unchanged; the compatibility converter is bypassed.

The host posts return conversions to the same session endpoint's `/codec` route.
Every call includes `owner_id`, `session_token`, `implementation_sha256` and
`operation`. The service must bind them to a pending preparation and return the
same `implementation_sha256`. Buffered `response` calls carry the actual provider
`body` and return a Messages `body`. Streaming uses `stream_start`, followed by
`stream_push` calls with one complete SSE `frame` and zero-based, monotonically
increasing `sequence`, then `stream_finish` with the next sequence. Push and
finish replies contain `frames`, an array of complete native SSE strings. Streams
and requests are bounded to 16 MiB; codec calls have a 10-second timeout and client
cancellation. Commit or abort must dispose of the service's codec stream state.

The host observes both protocols. It withholds a translated terminal until the
provider has supplied a finish reason and a complete `[DONE]` frame. It commits
only after the translated `message_stop` is accepted by the client transport,
using the exact delivered Messages assistant blocks for subsequent attribution.
An upstream HTTP connection left open after its terminal does not delay commit.
A rejected or failed codec, incomplete provider response or earlier client drop
aborts; a missing settlement acknowledgment quarantines the session as above.
The service owns signed/encrypted reasoning disposition and cache translation;
a codec must explicitly refuse unsupported or lossy cells instead of inventing
usage, signatures or private instructions.

`test/server_arc_codec.py` exercises this contract through a real isolated server
and a synthetic codec/provider. It runs in the router CI stages. This public
transport test does not establish any private codec's correctness, numerical ARC
parity, real Qwen inference, or the local llama.cpp subprocess seam. Those require
separate composition evidence against the selected package, codec and runtime.
Responses ingress remains unsupported by session collections.
