#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
wx_emoticon_decrypt.py —— 解密「群里别人发的表情包」为明文图片

原理：
  聊天图片/表情本地是 msg/attach/.../<md5>.dat 的加密文件，
  格式 V1：[6B sig][4B aes_size][4B xor_size][1B pad][aes][raw][xor]
  wechatauto.media.MediaDownloader 已实现解密（含密钥提取）。

用法:
  python wx_emoticon_decrypt.py --file "<dat路径>" --out "<输出目录>"
  python wx_emoticon_decrypt.py --list                 # 列出 attach 里所有 .dat
  python wx_emoticon_decrypt.py --all --out <目录>      # 全部解密
"""
import argparse
import glob
import json
import os
import sys

WX_ROOT = os.environ.get("DFY_WECHAT_ACCOUNT_DIR", "__account_not_configured__")
ATTACH = os.path.join(WX_ROOT, "msg", "attach")

IMG_SIGS = {
    b"\x89PNG": ".png",
    b"GIF8": ".gif",
    b"\xff\xd8\xff": ".jpg",
    b"RIFF": ".webp",
    b"BM": ".bmp",
}


def detect(path):
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except Exception:
        return None
    for sig, ext in IMG_SIGS.items():
        if head.startswith(sig):
            return ext
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--out", default="state/decoded")
    args = ap.parse_args()

    dats = sorted(glob.glob(os.path.join(ATTACH, "**", "*.dat"), recursive=True))
    if args.list:
        print("attach 里的 .dat 文件: %d 个" % len(dats))
        for d in dats:
            print("  %s  %d KB" % (d.replace(ATTACH, "..."), os.path.getsize(d) // 1024))
        return 0

    if not dats:
        print("没找到 .dat 文件")
        return 1

    targets = dats if args.all else ([args.file] if args.file else dats[:1])
    os.makedirs(args.out, exist_ok=True)

    from wechatauto import WeChatDB
    from wechatauto.media import MediaDownloader

    db = WeChatDB()
    print("账号: %s" % db.wxid)

    md = MediaDownloader(db, save_dir=args.out)
    print("探测图片密钥…")
    try:
        keys = md.detect_image_key()
        print("  密钥: aes=%s xor=%s" % (str(keys[0])[:16] + "…" if keys and keys[0] else None,
                                          keys[1] if keys else None))
    except Exception as e:
        print("  detect_image_key 失败: %s: %s" % (type(e).__name__, e))

    ok = fail = 0
    for i, src in enumerate(targets, 1):
        base = os.path.splitext(os.path.basename(src))[0]
        print("[%d/%d] %s" % (i, len(targets), base))
        try:
            data = md.decrypt_image(src)
        except Exception as e:
            print("     ❌ 异常: %s: %s" % (type(e).__name__, e))
            fail += 1
            continue
        if not data:
            print("     ❌ 解密返回空")
            fail += 1
            continue
        out = os.path.join(args.out, base + ".bin")
        with open(out, "wb") as f:
            f.write(data)
        ext = detect(out)
        if ext:
            final = os.path.join(args.out, base + ext)
            os.replace(out, final)
            print("     ✅ %s  (%d KB)" % (ext, len(data) // 1024))
            ok += 1
        else:
            print("     ⚠️ 解出 %d 字节但非图片格式 (head=%s)" % (len(data), data[:8].hex()))
            fail += 1

    print("")
    print("成功 %d / 失败 %d   输出: %s" % (ok, fail, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
