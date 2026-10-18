#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""export_favorites.py —— 把微信「当前收藏表情」导出成带 md5 的本地图片

为什么以 md5 命名：微信的收藏表情存在 `business/emoticon/Persist/` 下，**文件名就是 md5**。
用这个名字当身份，比事后按内容哈希反推可靠得多（早期流程把图重命名成 favorite_001.gif，
md5 就丢了，只能靠另外的映射表维系，用户一增删收藏就对不上）。

解密算法（微信 4.x，复用 CN-Grace 的解析器）：
  AES-128-CBC + PKCS7，key = IV = md5(f"{seed}{wxid}EMOTICON") 的 hex 前 16 字节
解密后的 payload 有两种：直接是 PNG/GIF/JPEG/WEBP，或者是 `wxgf` 容器（HEVC），需要解首帧。

用法：
  set DFY_WECHAT_ACCOUNT_DIR=<微信数据目录>\xwechat_files\wxid_xxx_1234
  python export_favorites.py --dry-run          # 只看会导出什么，不写文件
  python export_favorites.py --apply            # 真的写到 stickers/
  python export_favorites.py --apply --key-hex <32位hex>   # 派生失败时手工给 key

输出：一段 JSON 报告（总数 / 新增 md5 / 库里有但收藏没了的 md5 / 失败项）。
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import sys
from pathlib import Path

WECHAT = Path(__file__).resolve().parents[2]
STICKERS = Path(os.environ.get("DFY_STICKERS_DIR") or (WECHAT / "stickers"))
NATIVE = STICKERS / "native-md5.json"
PARSER = WECHAT / "vendor" / "wechat-emoticon-parser" / "wechat_emoticon_export.py"
SCRIPTS = WECHAT / "scripts"

SIGS = {b"\x89PNG": ".png", b"GIF8": ".gif", b"\xff\xd8\xff": ".jpg",
        b"RIFF": ".webp", b"BM": ".bmp"}


def account_dir(cli_value):
    value = cli_value or os.environ.get("DFY_WECHAT_ACCOUNT_DIR")
    if not value:
        raise SystemExit("缺少 --account-dir 或环境变量 DFY_WECHAT_ACCOUNT_DIR")
    path = Path(value)
    if not path.is_dir():
        raise SystemExit(f"账号数据目录不存在: {path}")
    return path


def ext_of(data):
    for sig, ext in SIGS.items():
        if data.startswith(sig):
            return ext
    return None


def decode_payload(plain, out_dir, md5):
    """把解密后的 payload 落成图片：普通图片直接写，wxgf(HEVC) 解首帧。"""
    ext = ext_of(plain)
    if ext:
        target = out_dir / f"{md5}{ext}"
        target.write_bytes(plain)
        return target, "raw"
    if plain.startswith(b"wxgf"):
        sys.path.insert(0, str(SCRIPTS))
        from emoji_receive_media import extract_wxgf   # 复用接收器的解帧逻辑
        target = out_dir / f"{md5}.png"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            extract_wxgf(plain, target)
        if target.is_file():
            return target, "wxgf"
    return None, None


def load_parser():
    spec = importlib.util.spec_from_file_location("emoticon_parser", PARSER)
    module = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        spec.loader.exec_module(module)
    return module


def resolve_key(persist_files, key_hex):
    if key_hex:
        return bytes.fromhex(key_hex)

    from wechatauto import WeChatDB
    module = load_parser()
    db = WeChatDB()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        seed = getattr(db, "cfg_dword", None)
        key = module.derive_key(seed, db.wxid) if seed else None
    if key and module.verify_key(key, persist_files[0].read_bytes()[:16]):
        return key
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--account-dir", help="微信账号数据目录（也可用 DFY_WECHAT_ACCOUNT_DIR）")
    parser.add_argument("--stickers-dir", help="表情库目录，默认 wechat-bridge/stickers")
    parser.add_argument("--key-hex", help="已知的 emoticon key（32 位 hex），跳过派生")
    args = parser.parse_args()
    if not (args.apply or args.dry_run):
        parser.error("请显式传 --dry-run 或 --apply")
    if args.stickers_dir:
        global STICKERS, NATIVE
        STICKERS = Path(args.stickers_dir)
        NATIVE = STICKERS / "native-md5.json"

    persist = account_dir(args.account_dir) / "business" / "emoticon" / "Persist"
    files = sorted(p for p in persist.rglob("*") if p.is_file())
    if not files:
        print(json.dumps({"status": "no_persist_files", "persist": str(persist)}, ensure_ascii=False))
        return 1

    from Crypto.Cipher import AES
    from Crypto.Util.Padding import unpad

    key = resolve_key(files, args.key_hex)
    if not key:
        print(json.dumps({
            "status": "key_derive_failed",
            "hint": "wechatauto 拿不到 cfg_dword 时，先用旧流程取一次 key，再用 --key-hex 传入",
        }, ensure_ascii=False))
        return 1

    known_md5 = set()
    try:
        payload = json.load(open(NATIVE, encoding="utf-8"))
        for item in (payload.get("stickers") or {}).values():
            known_md5.add(item["md5"].lower())
    except Exception:
        pass

    if args.apply:
        STICKERS.mkdir(parents=True, exist_ok=True)

    results, new_md5, bad = [], [], []
    for path in files:
        md5 = path.name.lower()
        try:
            plain = unpad(AES.new(key, AES.MODE_CBC, key).decrypt(path.read_bytes()), 16)
        except Exception as exc:
            bad.append({"md5": md5, "error": type(exc).__name__})
            continue
        content_md5 = hashlib.md5(plain).hexdigest()
        if args.apply:
            target, kind = decode_payload(plain, STICKERS, md5)
            if not target:
                bad.append({"md5": md5, "error": "undecodable_payload", "head": plain[:8].hex()})
                continue
            ext, size = target.suffix, target.stat().st_size
        else:
            kind = "wxgf" if plain.startswith(b"wxgf") else (ext_of(plain) or "?")
            ext, size = (".png" if kind == "wxgf" else f".{kind}"), len(plain)
        results.append({"md5": md5, "ext": ext, "bytes": size,
                        "content_md5": content_md5, "container": kind})
        if md5 not in known_md5:
            new_md5.append({"md5": md5, "ext": ext, "bytes": size, "container": kind})

    present = {r["md5"] for r in results}
    report = {
        "mode": "apply" if args.apply else "dry-run",
        "stickers_dir": str(STICKERS),
        "favorites_on_disk": len(files),
        "decoded": len(results),
        "failed": bad,
        "already_in_library": len(present & known_md5),
        "new": new_md5,
        "missing_but_in_library": sorted(known_md5 - present),
        "content_md5_equals_name": sum(1 for r in results if r["content_md5"] == r["md5"]),
    }
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
