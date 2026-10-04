# Anthropic-Compatible API

Lemonade supports an initial Anthropic Messages compatibility endpoint for applications that call Claude-style APIs.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /v1/messages` | Supported | Supports both streaming and non-streaming. Query params like `?beta=true` are accepted. |

Current scope focuses on message generation parity for common fields (`model`, `messages`, `system`, `max_tokens`, `temperature`, `stream`, and basic `tools`). Unsupported or unimplemented Anthropic-specific fields are ignored and surfaced via warning logs/headers.

Those limits apply only where Lemonade converts to and from the OpenAI shape. A cloud provider registered with `--wire-format anthropic` is relayed unconverted, so no field is dropped — see [Cloud Offload](../guide/configuration/cloud.md#providers-that-speak-the-anthropic-messages-format).

For converted local requests, `thinking.type` values `enabled` and `adaptive`
enable the backend's reasoning mode through `chat_template_kwargs.enable_thinking`.
`adaptive` uses that backend's default effort; Lemonade does not emulate
Anthropic's adaptive token budgeting and returns a compatibility warning.
`disabled` sets `enable_thinking: false` and `reasoning_effort: none`.
These controls require a backend and model template that support them.
Native Anthropic cloud relay retains the original controls unchanged.

When converting Messages history to an OpenAI-compatible local backend,
Lemonade keeps plain `thinking` text in `reasoning_content`. Anthropic signatures
have no destination in that field and are removed with a warning. Opaque
`redacted_thinking` cannot be translated and produces an explicit 400 response;
Lemonade does not fabricate readable reasoning from an encrypted block.

Local `reasoning_content` (or the `reasoning` string alias) becomes unsigned
Messages `thinking` content, including during streaming. Tool IDs, names and
argument fragments remain separate from text and thinking. A provider error or
incomplete stream ends with an error event rather than a successful
`message_stop`. These conversions do not assign a provider signature to local
reasoning. A routing integration that changes providers must apply its own
provenance-aware history policy before sending unsigned reasoning to a provider
that requires signed thinking. Native Anthropic relay continues to preserve the
original wire representation.

For converted responses, provider cache reads and writes are retained as
`cache_read_input_tokens` and `cache_creation_input_tokens`. Lemonade derives
uncached `input_tokens` from the total only when both cache counters are known
and consistent. Missing counters stay absent with a compatibility warning;
streaming requests ask the backend for final usage. This does not establish
that a backend honored the request's cache intent.

ARC session collections may opt into a pinned return-codec service for prepared
Chat destinations. That path applies private steering before provider encoding
and bypasses the compatibility conversions described above. See the
[ARC session codec contract](../dev/arc-router.md#messages-sessions-with-a-prepared-chat-provider)
for identity headers, destination binding, cache/reasoning responsibilities and
terminal settlement. Its transport support does not qualify a particular local
model or codec's behavior.
