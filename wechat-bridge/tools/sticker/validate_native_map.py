#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""validate_native_map.py —— 用当前账号的真实收藏校验 native-md5.json

背景：native-md5.json 是从某一时刻的「收藏快照」推出来的。但用户可能在微信里增删收藏 ——
实测过收藏数从 26 掉到 25，某条 md5 就不再唯一命中，表现是每次发送都先白跑一遍原生路径、
报 `target_not_unique_native_favorite`，再回落到发图片（看起来还能用，其实已经不是表情了）。

做法：对每条 md5 走一次 `/NativeEmoji/PrepareShape`（**只做收藏命中 + 对象构造 + 释放，不发送**），
命中的保留，没命中的从表里移出，并同步 meta.json 的 native 标记。

用法：
  python validate_native_map.py --dry-run
  python validate_native_map.py --apply
  python validate_native_map.py --apply --to filehelper

`--to` 只是构造消息对象的收件人，需要一个合法的会话 ID。默认 filehelper（文件传输助手）——
它不会打扰任何人。注意：**这条路径不会真的发出消息**（PrepareShape 只做准备）。
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

WECHAT = Path(__file__).resolve().parents[2]
STICKERS = Path(os.environ.get("DFY_STICKERS_DIR") or (WECHAT / "stickers"))
NATIVE = STICKERS / "native-md5.json"
META = STICKERS / "meta.json"
BASE = os.environ.get("DFY_HOOK_BASE", "http://127.0.0.1:30001")


def post(path, payload, timeout=40):
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(BASE + path, data=data,
                                     headers={"Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=timeout) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8", "replace")
        try:
            return error.code, json.loads(raw)
        except ValueError:
            return error.code, {"status": "http_error", "body": raw[:300]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--to", default="filehelper", help="构造消息对象的收件人（不会真发）")
    parser.add_argument("--stickers-dir", help="表情库目录，默认 wechat-bridge/stickers")
    args = parser.parse_args()
    if not (args.apply or args.dry_run):
        parser.error("请显式传 --dry-run 或 --apply")

    global STICKERS, NATIVE, META
    if args.stickers_dir:
        STICKERS = Path(args.stickers_dir)
        NATIVE = STICKERS / "native-md5.json"
        META = STICKERS / "meta.json"

    payload = json.load(open(NATIVE, encoding="utf-8"))
    stickers = payload.get("stickers") or {}
    if not stickers:
        print(json.dumps({"status": "native_map_empty", "path": str(NATIVE)}, ensure_ascii=False))
        return 1

    kept, dropped = {}, []
    examined = None

    for sticker_id, info in sorted(stickers.items(), key=lambda kv: kv[1].get("file", "")):
        code, result = post("/NativeEmoji/PrepareShape", {
            "to_wxid": args.to, "md5": info["md5"], "confirm": "prepare_shape_only"})
        if examined is None:
            examined = result.get("examined")
        ok = (result.get("matched") is True and result.get("prepared") is True
              and result.get("readback_verified") is True)
        if ok:
            kept[sticker_id] = info
        else:
            dropped.append({"file": info.get("file"), "md5": info["md5"],
                            "status": result.get("status") or f"http_{code}"})

    report = {"mode": "apply" if args.apply else "dry-run",
              "hook": BASE, "probe_recipient": args.to,
              "favorites_examined": examined,
              "before": len(stickers), "kept": len(kept), "dropped": dropped}

    if args.apply and dropped:
        stamp = datetime.now().strftime("%Y%m%d")
        NATIVE.with_name(NATIVE.name + f".bak-{stamp}").write_bytes(NATIVE.read_bytes())
        payload["stickers"] = kept
        payload["validated_at"] = stamp
        payload["validated_note"] = ("已用当前账号真实收藏核验；有 %d 条不再唯一命中，已移出。"
                                     % len(dropped))
        with open(NATIVE, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)

        if META.is_file():
            meta = json.load(open(META, encoding="utf-8"))
            for sticker_id, entry in (meta.get("stickers") or {}).items():
                entry["native"] = sticker_id in kept
                if sticker_id in kept:
                    entry["nativeMd5"] = kept[sticker_id]["md5"]
                else:
                    entry.pop("nativeMd5", None)
            with open(META, "w", encoding="utf-8") as handle:
                json.dump(meta, handle, ensure_ascii=False, indent=2)

    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
