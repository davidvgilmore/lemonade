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
