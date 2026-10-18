#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""decode_image.py —— 把聊天里的普通图片（<img>）解成本地可直接喂给模型的图

为什么不能像表情那样直接用 XML 里的 md5：
  `<img md5="8b57...">` 是**原图 md5**，而本地缓存的文件名是另一个哈希。
  正确映射在消息行的 `packed_info` 里（实测 local_id 71 -> d21273e4ac98799a7620cbee0fa5278e）。
  所以这里按 chat + local_id 反查本地 .dat 文件名，再用 wechatauto 的 MediaDownloader 解密。

用法:
  python decode_image.py --chat wxid_xxx --local-id 71 --out <目录>
输出 JSON：{"status":"ready","image_path":"...","bytes":N,"source":"..."}
只读本地缓存，不发消息。
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import re
import sys
from pathlib import Path

ACC = Path(os.environ.get("DFY_WECHAT_ACCOUNT_DIR", "__account_not_configured__"))
HEX32 = re.compile(r"[0-9a-f]{32}")

# 送进模型的图片上限（DSH 的 imageLimits 是部署配置，不能硬猜，所以自己先收敛得保守些）
MAX_SEND_BYTES = 900_000
MAX_SIDE = 1280


def fit_image(path, quality=88):
    """体积超限就等比缩小并转 JPEG。返回 (最终路径, 补充信息)。"""
    from PIL import Image

    p = Path(path)
    if p.stat().st_size <= MAX_SEND_BYTES:
        return p, None
    with Image.open(p) as src:
        image = src.convert("RGB")
        width, height = image.size
        scale = min(1.0, MAX_SIDE / max(width, height))
        if scale < 1.0:
            image = image.resize((max(1, int(width * scale)), max(1, int(height * scale))), Image.LANCZOS)
        target = p.with_suffix(".jpg")
        image.save(target, "JPEG", quality=quality, optimize=True)
        final_size = image.size
    return target, {"from_size": [width, height], "to_size": list(final_size),
                    "reason": "over_byte_limit"}


def local_name_for(db, chat, local_id):
    """从消息行里取出本地图片文件名（32 位十六进制）。"""
    # 1) 定向查询
    try:
        rows = db.get_message_rows_for_media(chat, int(local_id)) or []
    except Exception:
        rows = []
    if not rows:
        # 2) 退回整表扫描，按 local_id 精确匹配
        try:
            for row in db.get_image_rows(chat, limit=500) or []:
                if str(row.get("local_id")) == str(local_id):
                    rows = [row]
                    break
        except Exception:
            rows = []
    for row in rows:
        packed = row.get("packed_info")
        if packed is None:
            continue
        if isinstance(packed, bytes):
            packed = packed.decode("latin-1", errors="ignore")
        for hit in HEX32.findall(str(packed)):
            return hit
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--chat", required=True)
    parser.add_argument("--local-id", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--account", default=str(ACC))
    args = parser.parse_args()

    from wechatauto import WeChatDB
    from wechatauto.media import MediaDownloader

    account = Path(args.account)
    db = WeChatDB()
    name = local_name_for(db, args.chat, args.local_id)
    if not name:
        print(json.dumps({"status": "no_local_name", "sends_messages": False}, ensure_ascii=False))
        return 1

    chat_hash = hashlib.md5(args.chat.encode()).hexdigest()
    base = account / "msg" / "attach" / chat_hash
    candidates = []
    for suffix in ("", "_h", "_t"):
        candidates += sorted(base.glob(f"*/Img/{name}{suffix}.dat"))
    if not candidates:
        print(json.dumps({"status": "dat_missing", "local_name": name, "sends_messages": False},
                         ensure_ascii=False))
        return 1

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    downloader = MediaDownloader(db, save_dir=str(out_dir))

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from emoji_receive_media import extract_wxgf   # 复用接收器的 HEVC 解帧

    errors = []
    best = None

    def consider(path, data):
        """返回 (item 或 None)。普通图片直接落盘；wxgf 用 ffmpeg 解首帧。"""
        nonlocal best
        if data.startswith(b"wxgf"):
            # 微信把原图转成了 HEVC 容器，解一帧出来（分辨率比 _t 缩略图高得多）
            target = out_dir / (path.stem + ".png")
            info = extract_wxgf(data, target)
            if not target.is_file():
                errors.append(f"wxgf_extract_failed:{path.name}")
                return
            target, fitted = fit_image(target)
            item = {"image_path": str(target), "bytes": target.stat().st_size,
                    "md5": hashlib.md5(target.read_bytes()).hexdigest(),
                    "source": path.name, "container": "wxgf",
                    "preview_size": info.get("preview_size") if isinstance(info, dict) else None}
            if fitted:
                item["fitted"] = fitted
        else:
            if data.startswith(b"\x89PNG"):
                ext = ".png"
            elif data.startswith(b"\xff\xd8\xff"):
                ext = ".jpg"
            elif data.startswith(b"GIF8"):
                ext = ".gif"
            elif data.startswith(b"RIFF"):
                ext = ".webp"
            else:
                errors.append(f"unknown_format:{path.name}")
                return
            target = out_dir / (path.stem + ext)
            target.write_bytes(data)
            target, fitted = fit_image(target)
            item = {"image_path": str(target), "bytes": target.stat().st_size,
                    "md5": hashlib.md5(target.read_bytes()).hexdigest(), "source": path.name,
                    "container": "raw"}
            if fitted:
                item["fitted"] = fitted
        # 高清优先：按文件大小挑最大的一张
        if best is None or item["bytes"] > best["bytes"]:
            best = item

    for path in candidates:
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                data = downloader.decrypt_image(str(path))
        except Exception as exc:
            errors.append(f"{type(exc).__name__}:{path.name}")
            continue
        if not data:
            errors.append(f"empty:{path.name}")
            continue
        consider(path, data)

    if not best:
        print(json.dumps({"status": "decode_failed", "local_name": name,
                          "error_types": errors[:8], "sends_messages": False}, ensure_ascii=False))
        return 1
    best.update(status="ready", local_name=name, sends_messages=False)
    print(json.dumps(best, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
