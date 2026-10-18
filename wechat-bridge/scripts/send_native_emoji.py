"""Explicit-request-id native sticker client. Never automatically retries POST."""
from send_policy import authorize
import argparse
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:30001"


def call(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(BASE + path, data=data,
                                     headers={"Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=20) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8", errors="replace")
        try:
            result = json.loads(raw)
        except ValueError:
            result = {"status": "http_error", "body": raw[:1024]}
        return error.code, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    send = sub.add_parser("send")
    send.add_argument("--request-id", required=True)
    send.add_argument("--to", default="filehelper")
    send.add_argument("--md5", required=True)
    status = sub.add_parser("status")
    status.add_argument("--request-id", required=True)
    sub.add_parser("probe", help="Validate native executor after login; does not send")
    sub.add_parser("executor-status")
    args = parser.parse_args()
    if hasattr(args, "request_id") and not re.fullmatch(r"[a-z0-9_-]{8,64}", args.request_id):
        parser.error("request-id must be 8-64 lowercase ASCII letters, digits, _ or -")
    if args.command == "send" and not re.fullmatch(r"[0-9a-fA-F]{32}", args.md5):
        parser.error("md5 must contain 32 hex characters")
    if args.command == "send" and not re.fullmatch(
            r"filehelper|wxid_[A-Za-z0-9_-]{2,}|[0-9]{1,20}@chatroom", args.to):
        parser.error("--to must be filehelper, a wxid_, or <digits>@chatroom")
    if args.command == "send":
        try:
            authorize(args.to)
        except ValueError as error:
            parser.error(str(error))
        path, payload = "/NativeEmoji/Send", {
            "request_id": args.request_id, "to_wxid": args.to,
            "md5": args.md5.lower(), "confirm": "send_native_emoji"}
    elif args.command == "status":
        path, payload = "/NativeEmoji/SendStatus?" + urllib.parse.urlencode(
            {"request_id": args.request_id}), None
    elif args.command == "probe":
        path, payload = "/NativeExecutor/Probe", {}
    else:
        path, payload = "/NativeExecutor/Status", None
    try:
        code, result = call(path, payload)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "transport_or_response_error", "error": str(error),
                          "request_id": getattr(args, "request_id", None),
                          "action": "Query status using the SAME request_id. Do not create a new ID to retry."},
                         ensure_ascii=False, indent=2))
        return 2
    print(json.dumps({"http": code, "result": result}, ensure_ascii=False, indent=2))
    return 0 if 200 <= code < 300 else 1


if __name__ == "__main__":
    sys.exit(main())
