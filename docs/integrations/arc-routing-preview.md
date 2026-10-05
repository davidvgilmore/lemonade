# ARC routing integration preview

Use Lemonade with a local ARC routing encoder and cloud generation. ARC selects a
registered model and a per-turn steering action; it does not generate the answer.
The desktop app and Claude Code use the normal Lemonade interfaces. This guide
requires the private ARC operator distribution and its reviewed configuration;
Lemonade itself contains no ARC weights or private release settings.

This is an **integration preview**. Installed CLI flows have exercised real local
ARC decisions, cloud responses, tools and committed history. Native numerical
parity remains unqualified. A working integration is not a claim of benchmark
quality, equivalent scores, universal provider compatibility or durable recovery.

**Do not download, load or run a local Qwen3.8-27B generator for this preview.**
Generation stays in configured HTTPS cloud destinations, with no local fallback.
The small local routing encoder and heads are a separate dependency.

## Obtain the pinned private operator kit

The recorded kit targets macOS arm64 with its exact Python and Homebrew mbedTLS
prerequisites. It is not a universal Mac package. The index pins each dependency,
including the native distribution, session runtime, setup helper and an explicit
server/CLI split. Keep those identities rather than substituting current builds.

Use your existing authorized access to `rayline-ai/router-artifacts`. The kit is
preserved at revision `16952c51fcdc1500869fec76354e9a6fec4aec58`, under
`arc_local_integrations/20261004/checkpoint-202/`. Its `checkpoint.json` has SHA256
`bccb6bfd66f78eccbf3de3826961556427de14de2a5eb17487d94f7747777927`.
Retrieve and verify these members against that manifest, preserving their relative
layout below `operator-clean-install-202/kit/`:

- `operator_kit.py`
- `index.json`
- `artifacts/lemonade-tested-server-fixed-cli.tar.gz`

The existing Hugging Face download client consumes existing authentication only
when acquiring files. No credential-validation request is needed. The installed
runtime loads local verified files and does not require a Hugging Face token.
Never put a provider key in the index, exported JSON, command arguments or Git.

Set `KIT` to the verified kit directory and `WORK` to a new owned absolute parent
directory. Use an existing Python 3.13 with the Hugging Face client for acquisition;
the installer uses the separately pinned Python base for the runtime.

To bootstrap those three kit members with the existing Python download client,
set `KIT` to a new absolute destination and run this explicit acquisition step.
It verifies the pinned manifest and each selected file before writing the kit:

```sh
HF_HOME="$WORK/hf-cache" python3.13 - "$KIT" <<'PYTHON'
import hashlib, json, pathlib, sys
from huggingface_hub import hf_hub_download
revision = "16952c51fcdc1500869fec76354e9a6fec4aec58"
prefix = "arc_local_integrations/20261004/checkpoint-202/"
kit = "operator-clean-install-202/kit/"
def fetch(name):
    return pathlib.Path(hf_hub_download(
        repo_id="rayline-ai/router-artifacts", repo_type="dataset",
        revision=revision, filename=prefix + name)).read_bytes()
raw = fetch("checkpoint.json")
assert hashlib.sha256(raw).hexdigest() == "bccb6bfd66f78eccbf3de3826961556427de14de2a5eb17487d94f7747777927"
files = json.loads(raw)["files"]
out = pathlib.Path(sys.argv[1])
assert out.is_absolute()
out.mkdir(parents=True, exist_ok=False)
for name in ("operator_kit.py", "index.json", "artifacts/lemonade-tested-server-fixed-cli.tar.gz"):
    data = fetch(kit + name)
    expected = files[kit + name]
    assert len(data) == expected["bytes"]
    assert hashlib.sha256(data).hexdigest() == expected["sha256"]
    target = out / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(out / "checkpoint.json").write_bytes(raw)
PYTHON
```

After bootstrapping the kit, acquire its pinned components and install them:

```sh
python3.13 "$KIT/operator_kit.py" acquire --destination "$WORK/acquired"
python3.13 "$KIT/operator_kit.py" install-plan \
  --acquired "$WORK/acquired" --destination "$WORK/install" --host lemonade
python3.13 "$KIT/operator_kit.py" install \
  --acquired "$WORK/acquired" --destination "$WORK/install" --host lemonade
```

Use process-scoped `HF_HOME="$WORK/hf-cache"` for the kit's `acquire` command if
you want its downloads in that owned cache too. This does not modify your global
Hugging Face login or configuration.

Read the install plan before installing. Destinations must be absent; an error
leaves evidence rather than silently resuming a partial installation. Keep
`$WORK/install/python` stable: virtual environments refer to that Python base.
The setup helper is installed at its final location. Acquisition and installation
do not start a host or generate text. For preserved local checkpoints, acquisition
also accepts `--local-snapshots`, a JSON mapping of `native`, `session`, and `setup`
to their original checkpoint directories; this avoids downloading those bytes again.

The kit pins CLI `a460348f`, server `104801f7`, and matching server resources. This
split was exercised through an actual installed Claude Read/tool conversation.
It does not mean every executable was built from one commit. Exact Homebrew
library dependencies are prerequisites, not copied-library portability claims.

## Supply the catalog and export host configuration

Your private release configuration supplies `ARC_SETTINGS` and
`ARC_DESTINATIONS`. The former binds the complete 14-model, 70-action catalog;
the latter gives explicit cloud endpoint, provider model and credential environment
names. Do not copy a recording's loopback gateway settings into production or
remove actions to hide an unsupported profile. Endpoint configuration does not
establish provider availability or cache support.

```sh
ARC_INSTALL="$WORK/install"
ARC_PACKAGE="$ARC_INSTALL/native/release/package/package.json"
ARC_EXPORT="$WORK/export"
"$ARC_INSTALL/setup/arc-setup" \
  --settings "$ARC_SETTINGS" --package "$ARC_PACKAGE" \
  --destinations "$ARC_DESTINATIONS" --output "$ARC_EXPORT" \
  --package-alias private-release \
  --session-endpoint http://127.0.0.1:18102/experimental/arc/session \
  --owner my-lemonade-installation --name my_arc_router
"$ARC_INSTALL/session/arc-session" \
  --settings "$ARC_SETTINGS" --package "$ARC_PACKAGE" \
  --package-alias private-release --describe-config
```

Use new export directories. The helper derives opaque action bindings from the
package; do not transcribe or infer them from model names. Compare the configured
`codec_sha256` from `--describe-config` with the export report and session binding.
Changed settings require their new pin. Inspect unsupported-action reporting;
profile admission and cloud availability are separate facts.

The host consumes each configured provider key through its existing
`LEMONADE_<PROVIDER>_API_KEY` environment variable, or its ordinary in-memory cloud
auth interface. Use the exact names associated with the registered providers.
Supply keys only to the host process through your existing secret-loading method.
The client and routing services do not need the cloud key. Keys must not be saved
in the collection or desktop settings.

## Start the ordinary foreground services

Choose unused loopback ports and create owned state/cache/config directories.
Keep each foreground process in its own terminal so it can be stopped explicitly.

Routing service:

```sh
"$ARC_INSTALL/native/arc-native" serve \
  --sha256 cda9484f04c9689c8c19f8ed7d3799582c2c149d1019682c8b5104815e16e73a \
  --state-directory "$WORK/state/native" --port 18101 \
  --development-unqualified
```

Read `/health` on that port. Expect `qualification: unqualified-development`,
the recorded encoder identity and operational context limit 16384. This process
starts the routing encoder, not a local text generator.

Session service:

```sh
"$ARC_INSTALL/session/arc-session" \
  --settings "$ARC_SETTINGS" --package "$ARC_PACKAGE" \
  --package-alias private-release \
  --policy-endpoint http://127.0.0.1:18101/v1/rayline/arc/policy/decide \
  --port 18102
```

Use its actual startup receipt and configured codec pin. There is no session
health URL to invent and no need to generate an answer to check configuration.

Lemonade, with its cloud credentials already present in this process environment:

```sh
"$ARC_INSTALL/host/bin/lemond" --host 127.0.0.1 --port 18103 \
  --no-broadcast --log-file disabled \
  "$WORK/state/lemonade-cache" "$WORK/state/lemonade-config"
```

Keep matching `bin/resources` beside the server. Its ordinary health endpoint is
`http://127.0.0.1:18103/api/v1/health`. The desktop app is a client of this server;
it does not own the server or routing service lifecycle.

## Apply the export and use the router

For this explicitly owned, unauthenticated loopback server:

```sh
python3.13 "$KIT/operator_kit.py" apply-lemonade \
  --export "$ARC_EXPORT" --endpoint http://127.0.0.1:18103 \
  --receipts "$WORK/state/first-apply" \
  --cli "$ARC_INSTALL/host/bin/lemonade"
```

This posts the exported provider/model arrays through ordinary APIs and invokes
normal collection import with explicit host, port and `--no-discovery`. It does
not load models. An error preserves partial receipts and stops without automatic
retry or rollback. The narrow apply helper does not support authenticated host
application; use Lemonade's documented authenticated APIs for that deployment.

Open the browser at `http://127.0.0.1:18103/app`, or connect the desktop app to that
server. A separately supplied isolated native build can be launched directly:

```sh
"$ARC_DESKTOP_APP/Contents/MacOS/lemonade-app" \
  --isolated-profile "$WORK/profiles/new-desktop" \
  --server-url http://127.0.0.1:18103
```

The native app is a separately versioned artifact, not part of the kit above.
Use a new owned profile and the build with native output-setting persistence.
For manual import, use **File → New Hybrid Router → From JSON** and choose
`lemonade-collection.json`. Find **my_arc_router** and choose **Select router**.
Cloud-only routing requires no local generator download and does not preload
every candidate.

In **Settings → LLM → Maximum Output Tokens**, click Auto, enter **4096**, Save,
and reopen Settings to confirm it persisted. Then close Settings and ask:

> Which models are registered on this server? Use the model-list tool, then give
> a concise summary. Do not load or download any model.

The read-only tool returns registered models, not a remote availability probe.
The ordinary continuation carries its complete result. A previous real cloud
request exhausted 512 tokens on thinking without a visible answer; 4096 is the
preview recommendation. Output tokens and the routing encoder's 16384 projected
input-token capacity use different accounting. Neither is a dollar ceiling.

## Fresh Claude Code without global configuration

From the intended working directory, with the parent profile directory present:

```sh
CLAUDE_CODE_MAX_OUTPUT_TOKENS=4096 \
  "$ARC_INSTALL/host/bin/lemonade" \
  --host 127.0.0.1 --port 18103 --no-discovery \
  launch claude --model user.my_arc_router \
  --fresh-profile "$WORK/profiles/new-claude" --context-tokens 16384
```

The new profile must not already exist. Tool permissions remain interactive;
a fresh profile is not a filesystem sandbox. No global Claude or Ghostty
configuration edit is needed. Custom-model warnings may remain; the explicit
context budget does not prove tokenizer equivalence or tested compaction at the
boundary. See [Claude Code](claude-code.md) for launcher behavior and arguments.

## Routing, private steering and recovery

Stage 1 chooses among the complete eligible catalog at the session's model-change
boundaries. Stage 2 chooses the per-turn steering action for the held model.
The private session service applies the release's exact steering suffix only to
the provider request. It does not add that suffix to the visible conversation or
feed it back into the routing encoder. “Think more” and “think less” in preview
captions describe those private suffixes; they do not assert that a native
provider effort parameter changed. Native reasoning controls come from the
explicit release/profile binding, never from interpreting an action name.

The session codec preserves tool, thinking/signature and usage semantics within
its admitted formats, and commits the delivered client response. Do not edit
prior committed ARC messages: start a new chat for a new history. State and signed
history ownership are in memory. After an uncertain abort or worker loss, retain
the receipt and logs; restarting or changing a session ID does not prove recovery.

Finish or cancel the client, then stop the host, session service and routing
service in that order with their foreground terminals. Wait for each to exit.
Do not kill unrelated processes, restore another user's concurrently changed
profile, or automatically retry a request whose commit state is unknown.

## Keep the optional local example inactive

The separately supplied local-model proposal names `Qwen/Qwen3.8-27B` at immutable
revision `1d4bf0f2ff6012fd82039f2fa52739d0dd7c60c0`. It is **not an active model
registration**. Keep it in a separate, unimported file outside the cloud export;
no active candidate or action should refer to it. There is no invented
`enabled: false` escape hatch in the collection schema. Do not import, pull,
load or test that local model for this preview. Its proposed Chat profile and
fixed native reasoning baseline require separate qualification and cannot inherit
the cloud Messages profile's admission. Stage 2 steering remains a separate concept.

The private disabled-example supplement contains separate normal HTTP-provider,
model-registration and seven-action binding fragments, plus unchanged full-catalog
cloud configuration for comparison. Its static checks verify the exact seven
release action IDs and suffixes, all 70 active cloud actions, and zero references
to the proposed local endpoint from the active configuration. Do not apply or
merge these partial fragments: they have no qualified local session/codec binding.
No cloud native-admission identity is copied. The proposed native medium setting
remains independent of the private steering suffix and is not activated.

A possible quantized artifact is
[`ggml-org/Qwen3.8-27B-GGUF`](https://huggingface.co/ggml-org/Qwen3.8-27B-GGUF/tree/71bc7b627595dc8a91039addd9c791ae548d6747),
pinned at `71bc7b627595dc8a91039addd9c791ae548d6747`. Its publisher identifies the
Qwen model above as its base. Lemonade supports llama.cpp Metal in general, but
this artifact's compatibility with the installed runner, tokenizer/chat template,
tool and reasoning behavior, memory use and latency on this Mac remain unknown.
Only metadata was inspected. This is a future candidate, not a preview download
or launch instruction.

## Troubleshooting and preview limits

- **Download appears for the router:** check cloud registration, collection IDs
  and the cloud-readiness server/UI build. Do not download a generator as a workaround.
- **Pin or profile refusal:** re-export from the exact package and settings;
  preserve full catalog coverage. Do not substitute a previous capture's codec hash.
- **Tool continuation exceeds a limit:** retain the complete tool/history payload
  and inspect the actual boundary. A recording fixture's former 8 KiB byte cap
  was too small for a 210-entry list. Raising that fixture cap does not raise the
  runtime's 16384-token capacity or authorize catalog truncation.
- **Thinking appears without an answer:** inspect the actual stop reason and
  configured output budget. Do not call an exhausted response a successful answer.
- **Worker unavailable or transport error:** fail closed; do not fall back to a
  local generator or invent a completed transaction.
- **Cache or cost fields absent:** report them as unknown. A provider's reported
  usage/cost is evidence for that response, not a verified final invoice.

Preview recordings can be cropped and time-compressed for readability. Captions
must distinguish actual captured UI from editorial explanation. Browser,
model-free replay, native desktop and CLI results remain separate; a visible
answer does not retroactively turn a failed acceptance run into a pass. The
integration preview does not close native numerical parity or promise that every
configured provider has been exercised.
