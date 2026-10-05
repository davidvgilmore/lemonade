"""Opt-in ARC return-codec transport using synthetic loopback fixtures."""

import copy
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from utils.server_base import ServerTestBase, run_server_tests


class ArcCodecTests(ServerTestBase):
    def test_prepared_chat_return_codec(self):
        self._prepared_return_codec(False)

    def test_prepared_native_return_codec(self):
        self._prepared_return_codec(True)

    def _prepared_return_codec(self, native_provider):
        package = {"alias": "synthetic", "package_sha256": "b" * 64}
        action, pin = "a" * 64, "e" * 64
        state = {"wire": [], "prepared": [], "settled": [], "codec": []}
        release = threading.Event()
        self.addCleanup(release.set)
        native = {
            "id": "msg-test",
            "type": "message",
            "role": "assistant",
            "model": "worker",
            "content": [{"type": "text", "text": "done"}],
            "stop_reason": "end_turn",
            "usage": {"input_tokens": 2, "output_tokens": 1},
        }

        chat = {
            "id": "chat-test",
            "object": "chat.completion",
            "model": "worker",
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": "done"},
                    "finish_reason": "stop",
                }
            ],
        }
        chat_frames = [
            "data: "
            + json.dumps(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {"role": "assistant", "content": "done"},
                            "finish_reason": "stop",
                        }
                    ]
                }
            )
            + "\n\n",
            "data: [DONE]\n\n",
        ]
        source_format = "openai_chat" if native_provider else "anthropic_messages"
        target_format = "anthropic_messages" if native_provider else "openai_chat"

        def frame(kind, **fields):
            return (
                "event: "
                + kind
                + "\ndata: "
                + json.dumps({"type": kind, **fields})
                + "\n\n"
            )

        frames = [
            frame(
                "message_start", message={**native, "content": [], "stop_reason": None}
            ),
            frame("content_block_start", index=0, content_block=native["content"][0]),
            frame("content_block_stop", index=0),
            frame(
                "message_delta",
                delta={"stop_reason": "end_turn"},
                usage={"output_tokens": 1},
            ),
            frame("message_stop"),
        ]

        class Service(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, body):
                raw = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):  # pylint: disable=invalid-name
                self.reply({"data": [{"id": "worker", "object": "model"}]})

            def do_POST(self):  # pylint: disable=invalid-name
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path.endswith("/prepare"):
                    prepared = {
                        "model": "worker",
                        "messages": [
                            {"role": "user", "content": "private prepared text"}
                        ],
                        "stream": body["request"].get("stream", False),
                    }
                    if native_provider:
                        prepared["max_tokens"] = 100
                        prepared["thinking"] = {"type": "adaptive"}
                    if prepared["stream"] and not native_provider:
                        prepared["stream_options"] = {"include_usage": True}
                    state["prepared"].append(prepared)
                    self.reply(
                        {
                            "owner_id": "codec-host",
                            "package_sha256": package["package_sha256"],
                            "action_id": action,
                            "source_request_format": source_format,
                            "request_format": target_format,
                            "request": prepared,
                            "transaction_id": body["operation_id"],
                            "session_token": body["operation_id"],
                            "episode_id_hash": "c" * 64,
                            "context_epoch": "0",
                            "response_codec": {
                                "schema_version": "rayline.arc.response-codec.v1",
                                "source": target_format,
                                "target": source_format,
                                "implementation_sha256": pin,
                            },
                            "decision": {
                                "schema_version": "rayline.arc.policy-decision-response.v1",
                                "package": package,
                                "decision": {
                                    "selected_action_id": action,
                                    "selected_arm_id": "d" * 64,
                                },
                                "encoding": {"session_revision": 0},
                            },
                        }
                    )
                elif self.path.endswith("/codec"):
                    state["codec"].append(body)
                    operation = body["operation"]
                    result = {"implementation_sha256": pin}
                    if operation == "response":
                        result["body"] = chat if native_provider else native
                    elif operation in ("stream_push", "stream_finish"):
                        if native_provider:
                            event = body.get("frame", "")
                            result["frames"] = (
                                [chat_frames[0]]
                                if "message_delta" in event
                                else [chat_frames[1]] if "message_stop" in event else []
                            )
                        else:
                            result["frames"] = (
                                frames if "[DONE]" in body.get("frame", "") else []
                            )
                    self.reply(result)
                elif self.path.endswith(("/commit", "/abort")):
                    operation = self.path.rsplit("/", 1)[-1]
                    state["settled"].append((operation, body))
                    self.reply(
                        {"state": "committed" if operation == "commit" else "aborted"}
                    )
                else:
                    state["wire"].append(body)
                    state.setdefault("provider_headers", []).append(
                        {key.lower(): value for key, value in self.headers.items()}
                    )
                    state.setdefault("provider_paths", []).append(self.path)
                    response = {
                        "id": "chat-test",
                        "object": "chat.completion",
                        "model": "worker",
                        "choices": [
                            {
                                "index": 0,
                                "message": {"role": "assistant", "content": "done"},
                                "finish_reason": "stop",
                            }
                        ],
                    }
                    if not body.get("stream"):
                        self.reply(native if native_provider else response)
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    event = {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"role": "assistant", "content": "done"},
                                "finish_reason": "stop",
                            }
                        ]
                    }
                    raw = (
                        "data: " + json.dumps(event) + "\n\ndata: [DONE]\n\n"
                    ).encode()
                    if native_provider:
                        raw = "".join(frames).encode()
                    try:
                        for part in (raw[:7], raw[7:-1], raw[-1:]):
                            self.wfile.write(part)
                            self.wfile.flush()
                        release.wait(5)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

        service = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        thread = threading.Thread(target=service.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(service.server_close)
        self.addCleanup(service.shutdown)
        origin = f"http://127.0.0.1:{service.server_port}"
        candidate, collection = "user.TestArcCodec", "user.TestArcCodecRouter"
        for endpoint, body in (
            (
                "install",
                {
                    "backend": "cloud",
                    "provider": "testarccodec",
                    "base_url": origin + "/v1",
                    "wire_format": "anthropic" if native_provider else "openai",
                    "allow_insecure_http": True,
                },
            ),
            (
                "cloud/auth",
                {"provider": "testarccodec", "api_key": "synthetic-noncredential"},
            ),
            (
                "models/register",
                {
                    "model_name": candidate,
                    "recipe": "cloud",
                    "checkpoint": "worker",
                    "cloud_provider": "testarccodec",
                    "labels": ["chat"],
                },
            ),
        ):
            result = requests.post(
                self.base_url + "/" + endpoint, json=body, timeout=10
            )
            self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                self.base_url + "/uninstall",
                json={"backend": "cloud", "provider": "testarccodec"},
                timeout=10,
            )
        )
        self.addCleanup(
            lambda: requests.delete(
                self.base_url + "/cloud/auth/testarccodec", timeout=10
            )
        )
        self.addCleanup(
            lambda: requests.post(
                self.base_url + "/delete", json={"model_name": candidate}, timeout=10
            )
        )
        self.addCleanup(
            lambda: requests.post(
                self.base_url + "/delete", json={"model_name": collection}, timeout=10
            )
        )
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
                        "owner_id": "codec-host",
                        "codec_sha256": pin,
                    },
                    "actions": {
                        action: {
                            "model": candidate,
                            "wire_model": "worker",
                            "reasoning_effort": None,
                            "reasoning_max_tokens": None,
                            "steering_suffix": "",
                        }
                    },
                },
            },
        }
        result = requests.post(self.base_url + "/pull", json=policy, timeout=10)
        self.assertEqual(result.status_code, 200, result.text)
        source = {
            "model": collection,
            "max_tokens": 100,
            "messages": [{"role": "user", "content": "hello"}],
        }
        endpoint = self.base_url.removesuffix("/api/v1") + (
            "/v1/chat/completions" if native_provider else "/v1/messages"
        )
        for streaming in (False, True):
            body = copy.deepcopy(source)
            body["stream"] = streaming
            response = requests.post(
                endpoint,
                json=body,
                headers={
                    "X-Client-Session-Id": "codec-test",
                    "X-Lemonade-Request-Id": str(streaming),
                },
                timeout=3,
            )
            self.assertEqual(response.status_code, 200, response.text)
            if streaming:
                self.assertIn(
                    "[DONE]" if native_provider else "message_stop", response.text
                )
            else:
                actual = response.json()
                actual.pop("x_lemonade_route", None)
                self.assertEqual(actual, chat if native_provider else native)
            self.assertEqual(state["wire"][-1], state["prepared"][-1])
            self.assertEqual(
                state["provider_headers"][-1]["authorization"],
                "Bearer synthetic-noncredential",
            )
            self.assertTrue(
                state["provider_paths"][-1].endswith(
                    "/messages" if native_provider else "/chat/completions"
                )
            )
            if native_provider:
                self.assertEqual(
                    state["provider_headers"][-1]["anthropic-version"], "2023-06-01"
                )
        release.set()
        # A new prepare cannot pass the session gate before the prior commit ACK.
        response = requests.post(
            endpoint,
            json=source,
            headers={
                "X-Client-Session-Id": "codec-test",
                "X-Lemonade-Request-Id": "last",
            },
            timeout=3,
        )
        self.assertEqual(response.status_code, 200, response.text)
        self.assertTrue(all(kind == "commit" for kind, _ in state["settled"]))
        self.assertEqual(
            state["settled"][1][1]["response_messages"],
            [
                {
                    "role": "assistant",
                    "content": "done" if native_provider else native["content"],
                }
            ],
        )
        self.assertTrue(
            all(
                call["implementation_sha256"] == pin
                and call["owner_id"] == "codec-host"
                for call in state["codec"]
            )
        )


if __name__ == "__main__":
    run_server_tests(ArcCodecTests)
