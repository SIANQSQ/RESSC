"""HTTP GET controller. No automatic retries of motion commands."""
import argparse
import json
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def get_json(base, path, params=None):
    url = base.rstrip("/") + path
    if params:
        url += "?" + urlencode(params)
    request = Request(url, headers={"Cache-Control": "no-cache"})
    try:
        with urlopen(request, timeout=5) as response:
            return response.status, json.load(response)
    except HTTPError as error:
        return error.code, json.load(error)


def execute(base, action, params, result_timeout=5):
    code, body = get_json(base, "/api/" + action, params)
    if code == 202 and "id" in body:
        request_id = body["id"]
        deadline = time.monotonic() + result_timeout
        while time.monotonic() < deadline:
            time.sleep(0.05)
            code, body = get_json(base, "/api/result", {"id": request_id})
            if code != 202:
                return code, body
        return 202, {"pending": True, "id": request_id,
                     "message": "Execution still pending; query /api/result with this id."}
    return code, body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://esp32-stepper.local")
    parser.add_argument("action", choices=("status", "move", "move_all", "stop", "set", "home", "zero", "result"))
    parser.add_argument("params", nargs="*", metavar="key=value")
    args = parser.parse_args()
    params = {}
    for item in args.params:
        key, sep, value = item.partition("=")
        if not sep or not key or key in params:
            parser.error("parameters must be unique key=value pairs")
        params[key] = value
    try:
        code, body = execute(args.base, args.action, params)
    except (URLError, TimeoutError, OSError, ValueError) as error:
        print("Request failed; execution may be unknown. Check status before resending: " + str(error), file=sys.stderr)
        return 1
    print(json.dumps({"http_status": code, "body": body}, ensure_ascii=False, indent=2))
    return 0 if 200 <= code < 300 else 1


if __name__ == "__main__":
    sys.exit(main())
