"""Native ARC Chat transport through real lemond and synthetic loopback services.

Exercises session receipts and delivery settlement, not numerical ARC parity.
"""

import copy
from concurrent.futures import ThreadPoolExecutor
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from utils.server_base import ServerTestBase, run_server_tests


class ArcSessionTests(ServerTestBase):
    def test_native_chat_session(self):
        action = "a" * 64
        package = {"alias": "synthetic", "package_sha256": "b" * 64}
        wire_model = "synthetic/native-chat"
        state = {"prepared": [], "wire": [], "settled": [], "mode": "normal"}
        commit_waiting = threading.Event()
        release_commit = threading.Event()
        continuation_prepared = threading.Event()
        release_dropped_provider = threading.Event()
        release_stalled_provider = threading.Event()

        class Service(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, body, status=200):
                encoded = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def do_GET(self):  # pylint: disable=invalid-name
                self.reply({"data": [{"id": wire_model, "object": "model"}]})

            def do_POST(self):  # pylint: disable=invalid-name
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                mode = state["mode"]
                if self.path.endswith("/prepare"):
                    state["prepared"].append(body)
                    if body["operation_id"] == "continuation":
                        continuation_prepared.set()
                    prepared = copy.deepcopy(body["request"])
                    prepared["model"] = wire_model
                    prepared["messages"][-1][
                        "content"
                    ] += "\nprivate synthetic steering"
                    prepared["thinking"] = {"type": "enabled", "budget_tokens": 128}
                    receipt = {
                        "owner_id": "synthetic-host",
                        "package_sha256": package["package_sha256"],
                        "action_id": action,
                        "request_format": "openai_chat",
                        "source_request_format": "openai_chat",
                        "transaction_id": body["operation_id"],
                        "session_token": body["operation_id"],
                        "episode_id_hash": "c" * 64,
                        "context_epoch": "0",
                        "request": prepared,
                        "decision": {
                            "schema_version": "rayline.arc.policy-decision-response.v1",
                            "package": package,
                            "decision": {
                                "selected_action_id": action,
                                "selected_arm_id": "e" * 64,
                            },
                            "encoding": {"session_revision": 0},
                        },
                    }
                    if mode == "wrong-package":
                        receipt["package_sha256"] = "d" * 64
                    if mode == "invalid-arm":
                        receipt["decision"]["decision"]["selected_arm_id"] = "invalid"
                    if mode in (
                        "boolean-revision",
                        "negative-revision",
                        "float-revision",
                    ):
                        receipt["decision"]["encoding"]["session_revision"] = {
                            "boolean-revision": True,
                            "negative-revision": -1,
                            "float-revision": 1.5,
                        }[mode]
                    state["expected_wire"] = prepared
                    self.reply(receipt)
                elif self.path.endswith(("/commit", "/abort")):
                    operation = self.path.rsplit("/", 1)[-1]
                    if body["session_token"] == "lost-ack":
                        state["settled"].append((operation, body))
                        self.reply({"state": "unknown"}, 503)
                        return
                    if (
                        body["session_token"] == "delayed-first"
                        and operation == "commit"
                    ):
                        commit_waiting.set()
                        release_commit.wait(8)
                    state["settled"].append((operation, body))
                    self.reply(
                        {"state": "committed" if operation == "commit" else "aborted"}
                    )
                elif self.path.endswith("/chat/completions"):
                    state["wire"].append(body)
                    if mode == "provider-error":
                        self.reply({"error": {"message": "synthetic failure"}}, 500)
                        return
                    message = {
                        "role": "assistant",
                        "content": "answer",
                        "reasoning_content": "visible reasoning",
                    }
                    if not body.get("stream"):
                        self.reply(
                            {
                                "choices": [
                                    {
                                        "index": 0,
                                        "message": message,
                                        "finish_reason": "stop",
                                    }
                                ]
                            }
                        )
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    first = {
                        "choices": [
                            {"index": 0, "delta": message, "finish_reason": None}
                        ]
                    }
                    self.wfile.write(("data: " + json.dumps(first) + "\n\n").encode())
                    self.wfile.flush()
                    if mode == "stalled-drop":
                        release_stalled_provider.wait(8)
                        return
                    if mode == "client-drop":
                        release_dropped_provider.wait(8)
                        try:
                            extra = {
                                "choices": [
                                    {
                                        "index": 0,
                                        "delta": {"content": "x" * 262144},
                                        "finish_reason": None,
                                    }
                                ]
                            }
                            self.wfile.write(
                                ("data: " + json.dumps(extra) + "\n\n").encode()
                            )
                            self.wfile.flush()
                        except (BrokenPipeError, ConnectionResetError):
                            return
                    if mode not in ("partial-stream", "finish-only"):
                        final = {
                            "choices": [
                                {"index": 0, "delta": {}, "finish_reason": "stop"}
                            ]
                        }
                        try:
                            self.wfile.write(
                                (
                                    "data: "
                                    + json.dumps(final)
                                    + "\n\ndata: [DONE]\n\n"
                                ).encode()
                            )

                        except (BrokenPipeError, ConnectionResetError):
                            return
                    if mode == "finish-only":
                        final = {
                            "choices": [
                                {"index": 0, "delta": {}, "finish_reason": "stop"}
                            ]
                        }
                        self.wfile.write(
                            ("data: " + json.dumps(final) + "\n\n").encode()
                        )
                    self.wfile.flush()
                    if mode == "terminal-held-open":
                        release_stalled_provider.wait(8)

        service = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        thread = threading.Thread(target=service.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(service.server_close)
        self.addCleanup(service.shutdown)
        origin = f"http://127.0.0.1:{service.server_port}"
        provider = "testarcsession"
        for endpoint, payload in (
            (
                "install",
                {
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": origin + "/v1",
                    "allow_insecure_http": True,
                },
            ),
            (
                "cloud/auth",
                {
                    "provider": provider,
                    "api_key": "synthetic-key",
                    "allow_insecure_http": True,
                },
            ),
        ):
            result = requests.post(
                f"{self.base_url}/{endpoint}", json=payload, timeout=10
            )
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
        models = requests.get(f"{self.base_url}/models", timeout=10).json()["data"]
        candidate = next(m["id"] for m in models if m["id"].startswith(provider + "."))
        collection = "user.Test-Arc-Session"
        policy = {
            "model_name": collection,
            "version": "1",
            "recipe": "collection.router",
            "components": [candidate],
            "routing": {
                "candidates": [candidate],
                "default_model": candidate,
                "router": {
                    "type": "arc",
                    "package": package,
                    "session": {
                        "endpoint": origin + "/experimental/arc/session",
                        "owner_id": "synthetic-host",
                    },
                    "actions": {
                        action: {
                            "model": candidate,
                            "wire_model": wire_model,
                            "reasoning_effort": None,
                            "reasoning_max_tokens": 128,
                            "steering_suffix": "private synthetic steering",
                        }
                    },
                },
            },
        }
        result = requests.post(f"{self.base_url}/pull", json=policy, timeout=10)
        self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/delete", json={"model_name": collection}, timeout=10
            )
        )

        def wait_settled(count):
            deadline = time.monotonic() + 5
            while len(state["settled"]) < count and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertEqual(len(state["settled"]), count)

        original = {
            "model": collection,
            "messages": [{"role": "user", "content": "native prompt"}],
            "max_completion_tokens": 17,
        }
        missing = requests.post(
            f"{self.base_url}/chat/completions", json=original, timeout=10
        )
        self.assertEqual(missing.status_code, 502)
        self.assertEqual(state["prepared"], [])
        for endpoint in ("messages", "responses", "completions"):
            unsupported = requests.post(
                f"{self.base_url}/{endpoint}",
                json={**original, "max_tokens": 17},
                headers={
                    "X-Client-Session-Id": "conversation",
                    "X-Lemonade-Request-Id": "unsupported-" + endpoint,
                },
                timeout=10,
            )
            self.assertGreaterEqual(unsupported.status_code, 400)
            self.assertEqual(state["prepared"], [])
        for index, (stream, mode, expected) in enumerate(
            (
                (False, "normal", "commit"),
                (True, "normal", "commit"),
                (False, "provider-error", "abort"),
                (True, "partial-stream", "abort"),
                (False, "wrong-package", "abort"),
                (False, "invalid-arm", "abort"),
                (False, "boolean-revision", "abort"),
                (False, "negative-revision", "abort"),
                (False, "float-revision", "abort"),
            )
        ):
            state["mode"] = mode
            body = {**original, "stream": stream}
            result = requests.post(
                f"{self.base_url}/chat/completions",
                json=body,
                headers={
                    "X-Client-Session-Id": "conversation",
                    "X-Lemonade-Request-Id": f"operation-{index}",
                },
                timeout=15,
            )
            wait_settled(index + 1)
            operation, settled = state["settled"][-1]
            self.assertEqual(
                operation, expected, f"case {index}: {result.status_code} {result.text}"
            )
            self.assertEqual(state["prepared"][-1]["request"], body)
            self.assertEqual(settled["session_token"], f"operation-{index}")
            if expected == "commit":
                self.assertEqual(result.status_code, 200, result.text)
                self.assertEqual(state["wire"][-1], state["expected_wire"])
                self.assertNotIn("max_tokens", state["wire"][-1])
                self.assertNotIn("stream_options", state["wire"][-1])
                self.assertEqual(
                    settled["response_messages"],
                    [
                        {
                            "role": "assistant",
                            "content": "answer",
                            "reasoning_content": "visible reasoning",
                        }
                    ],
                )
                self.assertEqual(settled["settlement"], "successful_2xx_terminal_sent")

        state["mode"] = "normal"

        def send(operation_id, session_id="delayed-conversation"):
            return requests.post(
                f"{self.base_url}/chat/completions",
                json={**original, "stream": True},
                headers={
                    "X-Client-Session-Id": session_id,
                    "X-Lemonade-Request-Id": operation_id,
                },
                timeout=15,
            )

        first = send("delayed-first")
        self.assertEqual(first.status_code, 200, first.text)
        self.assertIn("[DONE]", first.text)
        self.assertTrue(commit_waiting.wait(3))
        with ThreadPoolExecutor(max_workers=2) as executor:
            continuation = executor.submit(send, "continuation")
            independent = executor.submit(send, "independent", "other-conversation")
            try:
                other = independent.result(timeout=5)
                self.assertEqual(other.status_code, 200, other.text)
                self.assertFalse(
                    continuation_prepared.wait(0.25),
                    "continuation prepared before previous commit ACK",
                )
            finally:
                release_commit.set()
            next_turn = continuation.result(timeout=5)
            self.assertEqual(next_turn.status_code, 200, next_turn.text)
        wait_settled(12)
        self.assertTrue(continuation_prepared.is_set())

        state["mode"] = "client-drop"
        dropped = requests.post(
            f"{self.base_url}/chat/completions",
            json={**original, "stream": True},
            headers={
                "X-Client-Session-Id": "drop-conversation",
                "X-Lemonade-Request-Id": "dropped",
            },
            stream=True,
            timeout=10,
        )
        self.assertEqual(dropped.status_code, 200)
        next(dropped.iter_content(chunk_size=32))
        dropped.close()
        try:
            wait_settled(13)
        finally:
            release_dropped_provider.set()
        self.assertEqual(
            state["settled"][-1],
            ("abort", {"owner_id": "synthetic-host", "session_token": "dropped"}),
        )

        state["mode"] = "normal"
        first = send("lost-ack", "uncertain-conversation")
        self.assertEqual(first.status_code, 200)
        wait_settled(14)
        count = len(state["prepared"])
        refused = send("after-lost-ack", "uncertain-conversation")
        self.assertEqual(refused.status_code, 502, refused.text)
        self.assertIn("unresolved", refused.text)
        self.assertEqual(len(state["prepared"]), count)
        self.assertEqual(len(state["settled"]), 14)

        state["mode"] = "terminal-held-open"
        started = time.monotonic()
        terminal = send("terminal-without-eof", "held-open-conversation")
        try:
            self.assertEqual(terminal.status_code, 200)
            self.assertLess(time.monotonic() - started, 4)
            state["mode"] = "normal"
            continuation = send("after-terminal-without-eof", "held-open-conversation")
            self.assertEqual(continuation.status_code, 200)
            wait_settled(16)
        finally:
            release_stalled_provider.set()

        state["mode"] = "finish-only"
        incomplete = send("finish-without-done", "finish-only-conversation")
        self.assertNotIn("[DONE]", incomplete.text)
        wait_settled(17)
        self.assertEqual(state["settled"][-1][0], "abort")

        release_stalled_provider.clear()
        state["mode"] = "stalled-drop"
        stalled = requests.post(
            f"{self.base_url}/chat/completions",
            json={**original, "stream": True},
            headers={
                "X-Client-Session-Id": "stalled-conversation",
                "X-Lemonade-Request-Id": "stalled-drop",
            },
            stream=True,
            timeout=10,
        )
        next(stalled.iter_content(chunk_size=32))
        stalled.close()
        try:
            wait_settled(18)
            self.assertEqual(state["settled"][-1][0], "abort")
        finally:
            release_stalled_provider.set()

        release_stalled_provider.clear()
        state["mode"] = "terminal-held-open"
        terminal_close = requests.post(
            f"{self.base_url}/chat/completions",
            json={**original, "stream": True},
            headers={
                "X-Client-Session-Id": "terminal-close-conversation",
                "X-Lemonade-Request-Id": "terminal-close",
            },
            stream=True,
            timeout=10,
        )
        try:
            for line in terminal_close.iter_lines(chunk_size=1):
                if line == b"data: [DONE]":
                    break
            else:
                self.fail("real terminal event was not delivered")
            terminal_close.close()
            wait_settled(19)
            self.assertEqual(state["settled"][-1][0], "commit")
        finally:
            terminal_close.close()
            release_stalled_provider.set()


if __name__ == "__main__":
    run_server_tests(ArcSessionTests, description="ARC NATIVE SESSION CONTRACT TESTS")
