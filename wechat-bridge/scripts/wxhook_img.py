#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
wxhook_img.py —— 通过 hook 内部 API 发送图片/表情包

微信的「表情包」（收藏表情）是加密存在内部库里的，没有现成文件路径，
所以策略是：把要发的图先落成本地文件（jpg/png/gif），再走 SendImgMsg。

用法:
  python wxhook_img.py --to 12345678901@chatroom --path "D:\\pic.png"
  python wxhook_img.py --to filehelper --path "D:\\pic.png" --json
"""
from send_policy import authorize
import argparse
import json
import os
import sys

import requests

BASE = "http://127.0.0.1:30001"
SESSION = requests.Session()
SESSION.trust_env = False


def send_image(to_id, path, timeout=60):
    if not os.path.isfile(path):
        return False, {"error": "file not found: %s" % path}
    authorize(to_id)
    payload = {"wxidorgid": to_id, "path": path}
    r = SESSION.post(BASE + "/SendImgMsg", json=payload, timeout=timeout)
    try:
        body = r.json()
        return r.status_code == 200 and isinstance(body, dict) and body.get("ret") == 0, body
    except Exception:
        return False, {"raw": r.text}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--to", required=True)
    ap.add_argument("--path", required=True)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    ok, body = send_image(args.to, args.path)
    if args.json:
        print(json.dumps(body, ensure_ascii=False))
    else:
        print("SendImgMsg → %s" % json.dumps(body, ensure_ascii=False))
        print("结果: %s" % ("✅ 已发送" if (isinstance(body, dict) and body.get("ret") == 0) else "❌ 失败"))
    return 0 if (isinstance(body, dict) and body.get("ret") == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
