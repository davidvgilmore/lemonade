"""Native ARC Messages through real lemond and synthetic loopback services."""

import copy
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from utils.server_base import ServerTestBase, run_server_tests


class ArcMessagesTests(ServerTestBase):
    def test_native_messages_session(self):
        action = "a" * 64
        other_action = "f" * 64
        package = {"alias": "synthetic", "package_sha256": "b" * 64}
        wire_model = "native-messages"
        state = {"prepared": [], "wire": [], "settled": [], "mode": "normal"}
        release = threading.Event()
        self.addCleanup(release.set)
        content = [
            {
                "type": "thinking",
                "thinking": "private thought",
                "signature": "opaque-signed",
            },
            {"type": "redacted_thinking", "data": "opaque-redacted"},
            {
                "type": "tool_use",
                "id": "tool-1",
                "name": "read",
                "input": {"path": "a"},
                "caller": {"type": "direct"},
            },
        ]
        assistant = {"role": "assistant", "content": content}

        class Service(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, body, status=200):
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):  # pylint: disable=invalid-name
                self.reply({"data": [{"id": wire_model, "object": "model"}]})

            def do_POST(self):  # pylint: disable=invalid-name
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                mode = state["mode"]
                if self.path.endswith("/prepare"):
                    state["prepared"].append(body)
                    prepared = copy.deepcopy(body["request"])
                    prepared["model"] = wire_model
                    prepared["messages"][-1]["content"].append(
                        {"type": "text", "text": "private steering"}
                    )
                    prepared["thinking"] = {"type": "enabled", "budget_tokens": 128}
                    state["expected"] = prepared
                    selected = other_action if mode == "unsupported-winner" else action
                    self.reply(
                        {
                            "owner_id": "native-host",
                            "package_sha256": package["package_sha256"],
                            "action_id": selected,
                            "request_format": "anthropic_messages",
                            "source_request_format": "anthropic_messages",
                            "transaction_id": body["operation_id"],
                            "session_token": body["operation_id"],
                            "episode_id_hash": "c" * 64,
                            "context_epoch": "0",
                            "request": prepared,
                            "decision": {
                                "schema_version": "rayline.arc.policy-decision-response.v1",
                                "package": package,
                                "decision": {
                                    "selected_action_id": selected,
                                    "selected_arm_id": "d" * 64,
                                },
                                "encoding": {"session_revision": 0},
                            },
                        }
                    )
                elif self.path.endswith(("/commit", "/abort")):
                    operation = self.path.rsplit("/", 1)[-1]
                    state["settled"].append((operation, body))
                    self.reply(
                        {"state": "committed" if operation == "commit" else "aborted"}
                    )
                elif self.path == "/v1/messages":
                    state["wire"].append((body, dict(self.headers)))
                    if mode == "error":
                        self.reply(
                            {
                                "type": "error",
                                "error": {"message": "synthetic failure"},
                            },
                            500,
                        )
                        return
                    if not body.get("stream"):
                        self.reply(
                            {
                                "id": "msg-1",
                                "type": "message",
                                "model": wire_model,
                                **assistant,
                                "stop_reason": "tool_use",
                                "usage": {
                                    "input_tokens": 3,
                                    "output_tokens": 5,
                                    "cache_creation_input_tokens": 7,
                                    "cache_read_input_tokens": 11,
                                },
                            }
                        )
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()

                    def event(kind, **fields):
                        data = {"type": kind, **fields}
                        try:
                            self.wfile.write(
                                (
                                    "event: "
                                    + kind
                                    + "\ndata: "
                                    + json.dumps(data)
                                    + "\n\n"
                                ).encode()
                            )
                            self.wfile.flush()
                            return True
                        except (BrokenPipeError, ConnectionResetError):
                            return False

                    event(
                        "message_start",
                        message={
                            "id": "msg-1",
                            "type": "message",
                            "role": "assistant",
                            "content": [],
                        },
                    )
                    if mode == "drop":
                        release.wait(8)
                        return
                    for index, block in enumerate(content):
                        initial = copy.deepcopy(block)
                        if block["type"] == "thinking":
                            initial.update(thinking="", signature="")
                        if block["type"] == "tool_use":
                            initial["input"] = {}
                        event("content_block_start", index=index, content_block=initial)
                        if block["type"] == "thinking":
                            event(
                                "content_block_delta",
                                index=index,
                                delta={
                                    "type": "thinking_delta",
                                    "thinking": block["thinking"],
                                },
                            )
                            event(
                                "content_block_delta",
                                index=index,
                                delta={
                                    "type": "signature_delta",
                                    "signature": block["signature"],
                                },
                            )
                        if block["type"] == "tool_use":
                            event(
                                "content_block_delta",
                                index=index,
                                delta={
                                    "type": "input_json_delta",
                                    "partial_json": '{"path":',
                                },
                            )
                            event(
                                "content_block_delta",
                                index=index,
                                delta={
                                    "type": "input_json_delta",
                                    "partial_json": '"a"}',
                                },
                            )
                        if mode == "unknown" and index == 0:
                            event(
                                "content_block_delta",
                                index=index,
                                delta={"type": "future_delta", "opaque": "value"},
                            )
                        event("content_block_stop", index=index)
                    event(
                        "message_delta",
                        delta={"stop_reason": "tool_use"},
                        usage={"output_tokens": 5},
                    )
                    if mode == "partial":
                        return
                    event("message_stop")
                    if mode == "held":
                        release.wait(8)

        service = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        thread = threading.Thread(target=service.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(service.server_close)
        self.addCleanup(service.shutdown)
        origin = f"http://127.0.0.1:{service.server_port}"
        provider = "testarcmessages"
        candidate = "user.Test-Arc-Messages-Worker"
        collection = "user.Test-Arc-Messages"
        for endpoint, body in (
            (
                "install",
                {
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": origin + "/v1",
                    "wire_format": "anthropic",
                    "allow_insecure_http": True,
                },
            ),
            ("cloud/auth", {"provider": provider, "api_key": "synthetic-key"}),
            (
                "models/register",
                {
                    "model_name": candidate,
                    "recipe": "cloud",
                    "checkpoint": wire_model,
                    "cloud_provider": provider,
                    "labels": ["chat"],
                },
            ),
        ):
            result = requests.post(f"{self.base_url}/{endpoint}", json=body, timeout=10)
            self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": provider},
                timeout=10,
            )
        )
        self.addCleanup(
            lambda: requests.delete(
                f"{self.base_url}/cloud/auth/{provider}", timeout=10
            )
        )
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/delete", json={"model_name": candidate}, timeout=10
            )
        )
        chat_provider = "testarcmessageschat"
        other_candidate = chat_provider + "." + wire_model
        for endpoint, body in (
            (
                "install",
                {
                    "backend": "cloud",
                    "provider": chat_provider,
                    "base_url": origin + "/v1",
                    "allow_insecure_http": True,
                },
            ),
            ("cloud/auth", {"provider": chat_provider, "api_key": "synthetic-key"}),
        ):
            result = requests.post(f"{self.base_url}/{endpoint}", json=body, timeout=10)
            self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/uninstall",
                json={"backend": "cloud", "provider": chat_provider},
                timeout=10,
            )
        )
        self.addCleanup(
            lambda: requests.delete(
                f"{self.base_url}/cloud/auth/{chat_provider}", timeout=10
            )
        )
        policy = {
            "model_name": collection,
            "version": "1",
            "recipe": "collection.router",
            "components": [candidate, other_candidate],
            "routing": {
                "candidates": [candidate, other_candidate],
                "default_model": candidate,
                "router": {
                    "type": "arc",
                    "package": package,
                    "session": {
                        "endpoint": origin + "/experimental/arc/session",
                        "owner_id": "native-host",
                    },
                    "actions": {
                        action: {
                            "model": candidate,
                            "wire_model": wire_model,
                            "reasoning_effort": None,
                            "reasoning_max_tokens": 128,
                            "steering_suffix": "private steering",
                        }
                    },
                },
            },
        }
        policy["routing"]["router"]["actions"][other_action] = {
            **policy["routing"]["router"]["actions"][action],
            "model": other_candidate,
        }
        result = requests.post(f"{self.base_url}/pull", json=policy, timeout=10)
        self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/delete", json={"model_name": collection}, timeout=10
            )
        )
        original = {
            "model": collection,
            "max_tokens": 256,
            "system": [
                {
                    "type": "text",
                    "text": "system",
                    "cache_control": {"type": "ephemeral", "ttl": "1h"},
                }
            ],
            "messages": [
                {"role": "user", "content": "Read the file."},
                assistant,
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "tool_result",
                            "tool_use_id": "tool-1",
                            "content": "result",
                        }
                    ],
                },
            ],
            "tools": [{"name": "read", "input_schema": {"type": "object"}}],
        }

        def wait_settled(count):
            deadline = time.monotonic() + 5
            while len(state["settled"]) < count and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertEqual(len(state["settled"]), count)

        def send(operation, stream=True, client_stream=False):
            return requests.post(
                f"{self.base_url.removesuffix('/api/v1')}/v1/messages",
                json={**original, "stream": stream},
                headers={
                    "X-Client-Session-Id": "native-conversation",
                    "X-Lemonade-Request-Id": operation,
                    "anthropic-beta": "synthetic-beta",
                    "anthropic-version": "2023-06-01",
                },
                stream=client_stream,
                timeout=12,
            )

        for index, (stream, mode, expected) in enumerate(
            (
                (False, "normal", "commit"),
                (True, "normal", "commit"),
                (True, "unknown", "commit"),
                (True, "partial", "abort"),
                (False, "error", "abort"),
                (True, "error", "abort"),
            )
        ):
            state["mode"] = mode
            result = send(str(index), stream)
            wait_settled(index + 1)
            operation, settled = state["settled"][-1]
            self.assertEqual(operation, expected, result.text)
            self.assertEqual(
                state["prepared"][-1]["request"], {**original, "stream": stream}
            )
            self.assertEqual(state["wire"][-1][0], state["expected"])
            self.assertEqual(
                set(state["prepared"][-1]["available_action_ids"]),
                {action, other_action},
            )
            self.assertEqual(
                state["wire"][-1][1].get("anthropic-beta"), "synthetic-beta"
            )
            if expected == "commit":
                self.assertEqual(
                    settled["response_messages"],
                    None if mode == "unknown" else [assistant],
                )
                if mode == "unknown":
                    self.assertEqual(settled["response_attribution"], "unknown")
                if not stream:
                    self.assertEqual(
                        result.json()["usage"]["cache_read_input_tokens"], 11
                    )

        state["mode"] = "held"
        started = time.monotonic()
        held = send("held", client_stream=True)
        try:
            for line in held.iter_lines(chunk_size=1):
                if line == b"event: message_stop":
                    continue
                if (
                    line.startswith(b"data:")
                    and json.loads(line[5:]).get("type") == "message_stop"
                ):
                    break
            else:
                self.fail("native terminal event absent")
            held.close()
            wait_settled(7)
            self.assertEqual(state["settled"][-1][0], "commit")
            self.assertLess(time.monotonic() - started, 4)
            state["mode"] = "normal"
            continuation = send("continuation")
            self.assertEqual(continuation.status_code, 200)
            wait_settled(8)
        finally:
            held.close()
            release.set()

        release.clear()
        state["mode"] = "drop"
        dropped = send("drop", client_stream=True)
        next(dropped.iter_content(chunk_size=16))
        dropped.close()
        try:
            wait_settled(9)
            self.assertEqual(state["settled"][-1][0], "abort")
        finally:
            release.set()

        state["mode"] = "unsupported-winner"
        wire_count = len(state["wire"])
        refused = send("unsupported-winner")
        wait_settled(10)
        self.assertEqual(refused.status_code, 502, refused.text)
        self.assertIn("native Messages", refused.text)
        self.assertEqual(state["settled"][-1][0], "abort")
        self.assertEqual(len(state["wire"]), wire_count)
        self.assertEqual(
            set(state["prepared"][-1]["available_action_ids"]), {action, other_action}
        )


if __name__ == "__main__":
    run_server_tests(ArcMessagesTests, description="ARC NATIVE MESSAGES CONTRACT TESTS")
