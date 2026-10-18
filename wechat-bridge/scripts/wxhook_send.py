#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
wxhook_send.py —— 通过 hook 内部 API 发送微信消息（不驱动界面）

原理：WeChat-Hook 的 version.dll 注入微信进程后，在 127.0.0.1:30001
     起了本地 HTTP 服务，直接调用微信内部函数发消息。

关键：必须用标准 UTF-8 JSON（实测 PowerShell 的 UTF-8 会变问号，
     而 GBK 会报 invalid json）。Python requests 的 json= 参数正确。

用法:
  python wxhook_send.py --to filehelper --text "你好"
  python wxhook_send.py --to wxid_xxx --text "你好" --json
  python wxhook_send.py --status              # 查状态
"""
from send_policy import authorize
import argparse
import json
import sys

import requests

BASE = "http://127.0.0.1:30001"
SESSION = requests.Session()
SESSION.trust_env = False


def post(path, payload=None, timeout=30):
    url = BASE + path
    r = SESSION.post(url, json=(payload or {}), timeout=timeout)
    try:
        return r.status_code, r.json()
    except Exception:
        return r.status_code, r.text


def get(path, timeout=15):
    r = SESSION.get(BASE + path, timeout=timeout)
    try:
        return r.status_code, r.json()
    except Exception:
        return r.status_code, r.text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--to", help="wxid / 群id / filehelper")
    ap.add_argument("--text", help="消息内容")
    ap.add_argument("--status", action="store_true", help="查登录/数据库状态")
    ap.add_argument("--json", action="store_true", help="输出原始 JSON")
    args = ap.parse_args()

    if args.status:
        code, body = get("/QueryDB/status")
        print("QueryDB/status → %s %s" % (code, json.dumps(body, ensure_ascii=False) if not isinstance(body, str) else body))
        code2, body2 = post("/QueryDB/GetAllDBName")
        n = len(body2) if isinstance(body2, list) else "?"
        print("GetAllDBName  → %s  数据库数=%s" % (code2, n))
        code3, body3 = post("/GetSelfProfile")
        if isinstance(body3, dict):
            print("GetSelfProfile→ wxid=%s nickname=%s" % (body3.get("wxid"), body3.get("nickname")))
        return 0

    if not args.to or args.text is None:
        print("需要 --to 和 --text（或用 --status）")
        return 2

    authorize(args.to)
    payload = {"wxidorgid": args.to, "msg": args.text}
    if args.json:
        print("请求: %s" % json.dumps(payload, ensure_ascii=False))

    code, body = post("/SendTextMsg", payload)
    ok = isinstance(body, dict) and body.get("ret") == 0
    print("SendTextMsg → HTTP %s  %s" % (code, json.dumps(body, ensure_ascii=False) if not isinstance(body, str) else body))
    print("结果: %s" % ("✅ 已发送" if ok else "❌ 失败"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
