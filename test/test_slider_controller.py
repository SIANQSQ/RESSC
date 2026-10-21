"""Protocol/state tests; does not open a GUI or connect to real motors."""
import copy
import importlib.util
import json
from pathlib import Path
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

MODULE = Path(__file__).resolve().parents[1] / "examples" / "slider_controller.py"
spec = importlib.util.spec_from_file_location("slider_controller", MODULE)
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def status():
    return {"auto_active": False, "self_test": "passed", "motors": [
        {"motor": i + 1, "moving": False, "homed": True, "fault": "",
         "position_mm": 0, "target_mm": 0, "length_mm": 100, "ppr": 6400}
        for i in range(4)]}


class Device:
    def __init__(self):
        self.state = status()
        self.calls = []
        self.result_code = 200

    def get(self, path, params=None):
        self.calls.append((path, params))
        if path == "/api/status":
            return 200, copy.deepcopy(self.state)
        if path == "/api/result":
            return self.result_code, {"ok": self.result_code == 200, "message": "mock result"}
        return 202, {"accepted": True, "id": len(self.calls)}

    def moves(self):
        return [params for path, params in self.calls if path == "/api/move_all"]


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.device, self.events = Device(), []
        self.now = 0
        self.control = gui.Controller(self.device, lambda *event: self.events.append(event), lambda: self.now)

    def test_cm_conversion_and_boundaries(self):
        p = gui.move_params([0, 1.25, 5, 10], 15)
        self.assertEqual([float(p["m%d" % i]) for i in range(1, 5)], [0, 12.5, 50, 100])
        for bad in ([-0.1, 0, 0, 0], [0, 0, 0, 10.1], [float("nan"), 0, 0, 0]):
            with self.assertRaises(ValueError):
                gui.move_params(bad, 15)
        self.control.step()
        self.assertFalse(self.device.moves())  # Connecting never moves to zero.

    def test_busy_coalesces_latest_and_checks_execution(self):
        self.device.state["motors"][0]["moving"] = True
        self.control.submit([1, 2, 3, 4], 15)
        self.control.step()
        self.control.submit([5, 6, 7, 8], 15)
        self.control.step()
        self.assertFalse(self.device.moves())
        self.device.state["motors"][0]["moving"] = False
        self.control.step()
        self.assertEqual(float(self.device.moves()[0]["m1"]), 50)
        self.assertEqual(float(self.device.moves()[0]["m4"]), 80)
        self.control.submit([9, 8, 7, 6], 15)
        self.control.step()  # Poll result, do not treat HTTP 202 as done.
        self.assertEqual(len(self.device.moves()), 1)
        self.now = 0.1
        self.control.step()  # Guard against stale idle snapshots after acceptance.
        self.assertEqual(len(self.device.moves()), 1)
        self.device.state["motors"][1]["moving"] = True
        self.now = 1
        self.control.step()
        self.assertEqual(len(self.device.moves()), 1)
        self.device.state["motors"][1]["moving"] = False
        self.control.step()
        self.assertEqual(float(self.device.moves()[1]["m1"]), 90)

    def test_stop_drops_pending_and_uses_get_stop(self):
        self.control.submit([1, 2, 3, 4], 15)
        self.control.step()
        self.control.submit([5, 6, 7, 8], 15)
        self.control.stop()
        self.assertIsNone(self.control.desired)
        self.control.step()
        self.assertIn(("/api/stop", {}), self.device.calls)
        self.assertTrue(any(kind == "stopped" for kind, _ in self.events))
        self.now = 5
        self.control.step()
        self.assertEqual(len(self.device.moves()), 1)

    def test_fault_reset_and_unreferenced_block_pending(self):
        for update in ("selftest", "fault", "zero"):
            self.device.state = status()
            self.control.submit([1, 2, 3, 4], 15)
            if update == "selftest":
                self.device.state["auto_active"] = True
            elif update == "fault":
                self.device.state["motors"][0]["fault"] = "soft travel limit"
            else:
                self.device.state["motors"][0]["position_mm"] = None
            self.control.step()
            self.assertFalse(self.device.moves())
            self.assertIsNone(self.control.desired)

    def test_range_and_dynamic_frequency_limit(self):
        self.device.state["motors"][3]["length_mm"] = 50
        self.control.submit([0, 0, 0, 6], 15)
        self.control.step()
        self.assertFalse(self.device.moves())
        self.device.state["motors"][0]["ppr"] = 12800
        self.control.submit([1, 2, 3, 4], 15)
        self.control.step()
        self.assertFalse(self.device.moves())
        self.control.submit([1, 2, 3, 4], 5)
        self.control.step()
        self.assertEqual(len(self.device.moves()), 1)

    def test_busy_result_retry_is_only_after_explicit_rejection(self):
        self.control.submit([1, 2, 3, 4], 15)
        self.control.step()
        self.device.result_code = 409
        self.control.step()
        self.assertIsNotNone(self.control.desired)
        self.device.state["motors"][0]["moving"] = True
        self.now = 1
        self.control.step()
        self.assertEqual(len(self.device.moves()), 1)
        self.device.state["motors"][0]["moving"] = False
        self.control.step()
        self.assertEqual(len(self.device.moves()), 2)

    def test_unresolved_result_is_not_resent(self):
        self.control.submit([1, 2, 3, 4], 15)
        self.control.step()
        self.device.result_code = 202
        self.now = 7
        with self.assertRaises(ValueError):
            self.control.step()
        self.assertEqual(len(self.device.moves()), 1)

    def test_http_transport_url_and_error_body(self):
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                received.append(urlsplit(self.path))
                self.send_response(400 if self.path.startswith("/bad") else 202)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"accepted": True, "id": 1}).encode())

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            client = gui.HttpClient("127.0.0.1:%d" % server.server_port)
            code, _ = client.get("/api/move_all", gui.move_params([0, 1, 5, 10], 15))
            self.assertEqual(code, 202)
            self.assertEqual(received[0].path, "/api/move_all")
            self.assertEqual(parse_qs(received[0].query)["m4"], ["100.000000"])
            self.assertEqual(client.get("/bad")[0], 400)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == "__main__":
    unittest.main()
