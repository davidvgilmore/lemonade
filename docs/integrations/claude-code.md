# Claude Code

Claude Code is Anthropic's coding agent CLI. With Lemonade, you can run Claude Code against local models through Lemonade's Anthropic-compatible API.

This guide focuses on the most common launch flows.

## Prerequisites

1. Install Claude Code:

    ```bash
    curl -fsSL https://claude.ai/install.sh | bash
    ```

    or:

    ```bash
    npm install -g @anthropic-ai/claude-code
    ```

2. Make sure Lemonade Server is running (`lemond`).

## Launch Claude Code with Lemonade

Use:

```bash
lemonade launch claude [options]
```

Lemonade automatically configures Claude Code to use your local server.

## Use Case 1: First-time user (discover + import + launch)

If you are not sure which model to use yet, start with:

```bash
lemonade launch claude
```

You will get an interactive menu where you can:

- Select a recipe to import and launch.
- Browse downloaded models.
- Browse recommended llama.cpp models (download may be required), then launch.

All remote recipes in this flow are sourced from:
`https://github.com/lemonade-sdk/recipes`

## Use Case 2: You already know the model

If you already downloaded a model or already imported the recipe, skip the interactive flow:

```bash
lemonade launch claude -m Qwen3.5-35B-A3B-GGUF
```

Equivalent long form:

```bash
lemonade launch claude --model Qwen3.5-35B-A3B-GGUF
```

When `--model` is provided, launch goes straight to starting the agent and loading that model.

## Passing Claude arguments with `--agent-args`

You can pass any extra Claude CLI flags through Lemonade:

```bash
lemonade launch claude --model Qwen3.5-35B-A3B-GGUF --agent-args "--approval-mode never"
```

Resume a previous Claude session:

```bash
lemonade launch claude --agent-args="--resume 6a670b84-ac78-47e0-8c2a-c3e85efdd979"
```

If Claude supports a flag, you can pass it through `--agent-args`.

## Related CLI Docs

For more launch examples and full option details, see:
`docs/lemonade-cli.md`

For Claude Code product details, see Anthropic's docs:
https://code.claude.com/docs/en/overview

## A fresh profile and an explicit context budget

For a separate client profile, pass a **new absolute directory** whose parent
already exists. Lemonade refuses to reuse an existing directory and leaves the
profile on disk after Claude exits:

```bash
lemonade launch claude --model user.MyCloudRouter \
  --fresh-profile /absolute/path/to/new-claude-profile \
  --context-tokens 16384
```

Use an already configured cloud router when generation must stay in the cloud;
these options do not change or install its backends. Normal model loading still
occurs, so select the intended router explicitly rather than choosing a local
model from the interactive menu. For the ARC cloud demo, only the small routing
encoder and heads run locally; generation stays in the configured cloud
providers. Do not load a local Qwen3.8-27B generator or configure a local
generation fallback.

`--fresh-profile` changes only the launched child's home, Claude configuration,
platform configuration/cache and temporary directories. It disables the default
Claude settings sources with `--setting-sources ""`; it does not edit your usual
Claude or Ghostty configuration. This requires a Claude version supporting that
flag. The working directory and tool permissions remain yours: a fresh profile
is not a filesystem sandbox. Explicit `--agent-args` can still select settings or
other client behavior. Claude may show first-run prompts in the new profile.

`--context-tokens` sets `CLAUDE_CODE_MAX_CONTEXT_TOKENS` only for the launched
process. Choose the budget supported by your deployment; 16384 above is an
example, not a default for all routers. This does not resize the server's context
or guarantee token equivalence between Claude and the routing encoder. Without
the option, the existing environment and Claude defaults are unchanged. Custom
model names may still produce an “unknown model” warning; the explicit budget
provides the client override, not model-catalog registration. Automatic
compaction at that limit has not been validated by this launcher change.
