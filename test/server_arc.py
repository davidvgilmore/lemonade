"""ARC contract smoke through a real lemond and a synthetic local policy worker.

No model weights, provider calls, or numerical inference parity are exercised.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from server_router import start_mock_cloud_provider
from utils.server_base import ServerTestBase, run_server_tests


class ArcRouterTests(ServerTestBase):
    def test_arc_native_validation(self):
        action = "a" * 64
        package = {"alias": "synthetic", "package_sha256": "b" * 64}
        request = {
            "schema_version": "rayline.arc.policy-decision-request.v1",
            "package": package,
            "episode_id_hash": "c" * 64,
            "context_epoch": "epoch-0",
            "request_format": "openai_chat",
            "request": {
                "messages": [
                    {"role": "system", "content": "preserve\nbytes"},
                    {"role": "user", "content": "synthetic request"},
                ]
            },
            "attribution": [],
            "selection": {"available_action_ids": [action]},
        }
        response = {
            "schema_version": "rayline.arc.policy-decision-response.v1",
            "package": package,
            "decision": {"selected_action_id": action},
            "encoding": {"representation_id": "synthetic"},
        }
        state = {"response": response, "requests": []}

        class Worker(BaseHTTPRequestHandler):
            def do_POST(self):  # pylint: disable=invalid-name
                state["requests"].append(
                    json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                )
                payload = json.dumps(state["response"]).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        worker = ThreadingHTTPServer(("127.0.0.1", 0), Worker)
        thread = threading.Thread(target=worker.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(worker.server_close)
        self.addCleanup(worker.shutdown)
        policy = {
            "version": "1",
            "recipe": "collection.router",
            "components": ["synthetic-candidate"],
            "routing": {
                "candidates": ["synthetic-candidate"],
                "default_model": "synthetic-candidate",
                "router": {
                    "type": "arc",
                    "endpoint": f"http://127.0.0.1:{worker.server_port}/v1/rayline/arc/policy/decide",
                    "package": package,
                    "actions": {
                        action: {
                            "model": "synthetic-candidate",
                            "reasoning_effort": "high",
                            "reasoning_max_tokens": None,
                            "steering_suffix": "",
                        }
                    },
                },
            },
        }
        for prefix in ("/api/v0", "/api/v1", "/v0", "/v1"):
            base = self.base_url.removesuffix("/api/v1")
            result = requests.post(
                f"{base}{prefix}/routing/validate",
                json={"policy": policy, "arc_request": request},
                timeout=10,
            )
            self.assertEqual(result.status_code, 200, result.text)
            decision = result.json()["decision"]
            self.assertEqual(decision["route_to"], "synthetic-candidate")
            self.assertFalse(decision["default_used"])
            self.assertEqual(decision["outputs"]["arc"], response)
            self.assertEqual(state["requests"][-1], request)
        receipt = {}
        provider = "testarclocal"
        upstream = "synthetic/arc-target"
        collection = "user.Test-Arc-Contract"
        base_url, stop_provider = start_mock_cloud_provider(
            [upstream], "synthetic-answer", receipt
        )
        self.addCleanup(stop_provider)
        for endpoint, payload in (
            (
                "install",
                {
                    "backend": "cloud",
                    "provider": provider,
                    "base_url": base_url,
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
        candidate = next(m["id"] for m in models if m["id"].startswith(f"{provider}."))
        policy["model_name"] = collection
        policy["components"] = [candidate]
        policy["routing"]["candidates"] = [candidate]
        policy["routing"]["default_model"] = candidate
        policy["routing"]["router"]["actions"][action]["model"] = candidate
        result = requests.post(f"{self.base_url}/pull", json=policy, timeout=10)
        self.assertEqual(result.status_code, 200, result.text)
        self.addCleanup(
            lambda: requests.post(
                f"{self.base_url}/delete", json={"model_name": collection}, timeout=10
            )
        )
        body = {
            "model": collection,
            "route_trace": True,
            "max_tokens": 8,
            "messages": request["request"]["messages"],
            "arc_context": {
                key: value for key, value in request.items() if key != "request"
            },
        }
        result = requests.post(
            f"{self.base_url}/chat/completions", json=body, timeout=10
        )
        self.assertEqual(result.status_code, 200, result.text)
        self.assertEqual(result.json()["x_lemonade_route"]["outputs"]["arc"], response)
        self.assertEqual(state["requests"][-1], request)
        self.assertEqual(receipt["last_chat_request"]["model"], upstream)
        self.assertEqual(receipt["last_chat_request"]["messages"], body["messages"])
        self.assertEqual(receipt["last_chat_request"]["reasoning_effort"], "high")
        self.assertNotIn("arc_context", receipt["last_chat_request"])
        receipt.clear()
        for invalid in (
            {**response, "package": {**package, "package_sha256": "d" * 64}},
            {**response, "decision": {"selected_action_id": "d" * 64}},
            {**response, "schema_version": "unsupported"},
        ):
            state["response"] = invalid
            result = requests.post(
                f"{self.base_url}/routing/validate",
                json={"policy": policy, "arc_request": request},
                timeout=10,
            )
            self.assertEqual(result.status_code, 400, result.text)
            self.assertNotIn("decision", result.json())
            result = requests.post(
                f"{self.base_url}/chat/completions", json=body, timeout=10
            )
            self.assertEqual(result.status_code, 502, result.text)
            self.assertEqual(receipt, {})

        state["response"] = response
        policy["routing"]["router"]["actions"][action][
            "steering_suffix"
        ] = "synthetic steering"
        result = requests.post(f"{self.base_url}/pull", json=policy, timeout=10)
        self.assertEqual(result.status_code, 200, result.text)
        result = requests.post(
            f"{self.base_url}/routing/validate",
            json={"policy": policy, "arc_request": request},
            timeout=10,
        )
        self.assertEqual(result.status_code, 200, result.text)
        before = len(state["requests"])
        result = requests.post(
            f"{self.base_url}/chat/completions", json=body, timeout=10
        )
        self.assertEqual(result.status_code, 502, result.text)
        self.assertEqual(len(state["requests"]), before)
        self.assertEqual(receipt, {})


if __name__ == "__main__":
    run_server_tests(ArcRouterTests, description="ARC CONTRACT TESTS")
