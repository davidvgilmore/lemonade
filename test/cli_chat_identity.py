"""Synthetic HTTP acceptance for conversation identity on the ordinary CLI."""

import json
import os
import subprocess
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class ChatIdentityTest(unittest.TestCase):
    def test_history_reset_and_independent_repls(self):
        receipts = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, body, content_type="application/json"):
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.reply(json.dumps({"id": "synthetic", "downloaded": True}))

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path.endswith("/chat/completions"):
                    receipts.append((dict(self.headers), body))
                    response = {
                        "choices": [
                            {"message": {"role": "assistant", "content": "answer"}}
                        ]
                    }
                    if body["stream"]:
                        chunk = {
                            "choices": [
                                {"delta": {"content": "answer"}, "finish_reason": None}
                            ]
                        }
                        self.reply(
                            "data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n",
                            "text/event-stream",
                        )
                    else:
                        self.reply(json.dumps(response))
                else:
                    self.reply("{}")

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        binary = os.environ.get(
            "LEMONADE_CLI",
            str(Path(__file__).resolve().parents[1] / "build" / "lemonade"),
        )
        try:
            for flags in ([], ["--no-stream"]):
                subprocess.run(
                    [
                        binary,
                        "--port",
                        str(server.server_port),
                        "chat",
                        "synthetic",
                        *flags,
                    ],
                    input="one\ntwo\n/clear\nthree\n/system system\nfour\n/quit\n",
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=True,
                    timeout=20,
                )
            self.assertEqual(len(receipts), 8)
            sessions = [h["X-Client-Session-Id"] for h, _ in receipts]
            operations = [h["X-Lemonade-Request-Id"] for h, _ in receipts]
            self.assertEqual(len(set(operations)), 8)
            for start in (0, 4):
                self.assertEqual(sessions[start], sessions[start + 1])
                self.assertNotEqual(sessions[start + 1], sessions[start + 2])
                self.assertNotEqual(sessions[start + 2], sessions[start + 3])
                self.assertEqual(
                    receipts[start + 1][1]["messages"][1],
                    {"role": "assistant", "content": "answer"},
                )
                self.assertEqual(len(receipts[start + 2][1]["messages"]), 1)
            self.assertTrue(set(sessions[:4]).isdisjoint(sessions[4:]))
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
