#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""decode_sticker.py —— 按 md5 把群里收到的表情解码成首帧 PNG（复用接收器同一套逻辑）

输出 JSON：{"status":"ready","image_path":"...","library_number":"122",...}
不解密远程 URL、不发消息、不写密钥到输出。

用法:
  python decode_sticker.py --md5 <32hex> --chat 12345678901@chatroom --out <目录>
"""
import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

ACCOUNT = os.environ.get("DFY_WECHAT_ACCOUNT_DIR", "")
CATALOG = HERE.parent / "stickers" / "检索库素材" / "sticker-catalog.json"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--md5", required=True)
    parser.add_argument("--chat", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--account", default=ACCOUNT)
    parser.add_argument("--catalog", default=str(CATALOG))
    args = parser.parse_args()
    if not args.account:
        parser.error("Set DFY_WECHAT_ACCOUNT_DIR or --account")

    from emoji_receive_media import EmojiMedia
    from wechatauto import WeChatDB

    db = WeChatDB()
    media = EmojiMedia(args.account, args.out, catalog=args.catalog, db=db)
    result = media.resolve(args.md5.lower(), args.chat)
    image = Path(args.out) / args.md5.lower() / "frame.png"
    if result.get("status") == "ready" and image.is_file():
        result["image_path"] = str(image)
        result["bytes"] = image.stat().st_size
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result.get("status") == "ready" else 1


if __name__ == "__main__":
    sys.exit(main())
