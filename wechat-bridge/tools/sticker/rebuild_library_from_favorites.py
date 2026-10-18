#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""rebuild_library_from_favorites.py —— 用「当前微信收藏」重建表情库索引

为什么需要重建：如果早期用 `093.jpg / 122.gif` 这种顺序命名，身份（md5）只存在于另外的
映射文件里，用户一增删收藏就对不上。这里改成 **md5 命名文件**，索引每次都能从收藏目录重算。

做四件事：
  1. 以 Persist 目录里的 md5 列表为准，找到 export_favorites.py 导出的图
  2. 重建 meta.json：sticker id = md5 前 16 位，file = <md5>.<ext>，旧描述按 md5 继承
  3. 重建 native-md5.json（每条都是当前收藏，因此可用于原生 type47 发送）
  4. 把不再被引用的顺序命名旧图归档到 `_archive-sequential-<时间>`，不删除

用法：
  python rebuild_library_from_favorites.py --dry-run
  python rebuild_library_from_favorites.py --apply

前置：先跑 `export_favorites.py --apply`，让 stickers/ 里出现 <md5>.png|gif|jpg。
"""
import argparse
import json
import os
import shutil
import sys
from datetime import datetime
from pathlib import Path

WECHAT = Path(__file__).resolve().parents[2]
STICKERS = Path(os.environ.get("DFY_STICKERS_DIR") or (WECHAT / "stickers"))
META = STICKERS / "meta.json"
NATIVE = STICKERS / "native-md5.json"

IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg", ".gif", ".webp")


def account_dir(cli_value):
    value = cli_value or os.environ.get("DFY_WECHAT_ACCOUNT_DIR")
    if not value:
        raise SystemExit("缺少 --account-dir 或环境变量 DFY_WECHAT_ACCOUNT_DIR")
    path = Path(value)
    if not path.is_dir():
        raise SystemExit(f"账号数据目录不存在: {path}")
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--account-dir", help="微信账号数据目录（也可用 DFY_WECHAT_ACCOUNT_DIR）")
    parser.add_argument("--stickers-dir", help="表情库目录，默认 wechat-bridge/stickers")
    args = parser.parse_args()
    if not (args.apply or args.dry_run):
        parser.error("请显式传 --dry-run 或 --apply")

    global STICKERS, META, NATIVE
    if args.stickers_dir:
        STICKERS = Path(args.stickers_dir)
        META = STICKERS / "meta.json"
        NATIVE = STICKERS / "native-md5.json"

    persist = account_dir(args.account_dir) / "business" / "emoticon" / "Persist"
    md5s = sorted(p.name.lower() for p in persist.rglob("*") if p.is_file())
    if not md5s:
        print(json.dumps({"status": "no_persist_files", "persist": str(persist)}, ensure_ascii=False))
        return 1

    old_meta = {}
    try:
        old_meta = json.load(open(META, encoding="utf-8")).get("stickers") or {}
    except Exception:
        pass

    # 旧条目按 nativeMd5 建索引，方便把描述搬到新的 md5 键上
    by_md5 = {}
    for entry in old_meta.values():
        md5 = (entry.get("nativeMd5") or "").lower()
        if md5:
            by_md5[md5] = entry

    entries, native_map, missing_file, inherited = {}, {}, [], 0
    for md5 in md5s:
        matches = sorted(p for p in STICKERS.glob(md5 + ".*") if p.suffix.lower() in IMAGE_SUFFIXES)
        if not matches:
            missing_file.append(md5)
            continue
        path = matches[0]
        sticker_id = md5[:16]
        previous = by_md5.get(md5) or {}
        if previous:
            inherited += 1
        entries[sticker_id] = {
            "file": path.name,
            "desc": previous.get("desc", ""),
            "localNote": previous.get("localNote", ""),
            "tags": previous.get("tags", []),
            "source": previous.get("source") or "微信收藏",
            "size": path.stat().st_size,
            "native": True,
            "nativeMd5": md5,
            "usedCount": previous.get("usedCount", 0),
            "lastUsedAt": previous.get("lastUsedAt", ""),
            "addedAt": previous.get("addedAt", ""),
        }
        native_map[sticker_id] = {"md5": md5, "file": path.name}

    unannotated = sorted(e["file"] for e in entries.values() if not e["desc"])
    report = {
        "mode": "apply" if args.apply else "dry-run",
        "stickers_dir": str(STICKERS),
        "favorites": len(md5s),
        "rebuilt_entries": len(entries),
        "inherited_annotations": inherited,
        "needs_annotation": len(unannotated),
        "needs_annotation_files": unannotated,
        "missing_image_file": missing_file,
    }

    if args.apply:
        stamp = datetime.now().strftime("%Y%m%d")
        for existing in (META, NATIVE):
            if existing.is_file():
                existing.with_name(existing.name + f".bak-{stamp}").write_bytes(existing.read_bytes())

        keep = {e["file"] for e in entries.values()}
        archive = STICKERS / f"_archive-sequential-{stamp}"
        archived = 0
        for path in list(STICKERS.iterdir()):
            if not path.is_file() or path.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            if path.name in keep:
                continue
            archive.mkdir(parents=True, exist_ok=True)
            shutil.move(str(path), str(archive / path.name))
            archived += 1
        report["archived_old_files"] = archived

        meta = {
            "version": 3,
            "rebuilt_at": stamp,
            "note": "文件名即表情 md5。sticker id = md5 前 16 位；只有这份表里的表情能发原生 type47。",
            "stickers": dict(sorted(entries.items(), key=lambda kv: kv[1]["file"])),
        }
        with open(META, "w", encoding="utf-8") as handle:
            json.dump(meta, handle, ensure_ascii=False, indent=2)

        payload = {
            "schema_version": 2,
            "rebuilt_at": stamp,
            "note": "来自当前微信收藏（Persist 目录，文件名即 md5）。改收藏后重跑导出与重建即可。",
            "stickers": dict(sorted(native_map.items(), key=lambda kv: kv[1]["file"])),
        }
        with open(NATIVE, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)

    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
