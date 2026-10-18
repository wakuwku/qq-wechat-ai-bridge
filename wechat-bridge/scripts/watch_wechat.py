#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
watch_wechat.py —— 只读监听微信群消息，写入队列（不发送任何东西）

支持多群：--chat 可以给多个（空格分隔），或逗号分隔。

用法:
  python watch_wechat.py --chat 12345678901@chatroom 12345678901@chatroom \
      --out state/queue.jsonl --cursor state/cursor.json --names state/names.json
  python watch_wechat.py --list            # 列出所有群
  python watch_wechat.py --chat X --once   # 跑一轮

设计要点（血泪教训）：
  - 水位(local_id) 按会话独立记录，F 盘换新数据目录后 lid 会从 1 重新开始，
    所以重新登录/换数据目录后必须删掉 cursor.json。
  - 首次监听某会话时，整轮只用来"立水位"，不回溯历史消息。
"""
import argparse
import json
import os
import re
import sys
import time
from datetime import datetime

from wechatauto import WeChatDB


def now_iso():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def fmt_time(ts):
    try:
        return datetime.fromtimestamp(int(ts)).strftime("%m-%d %H:%M:%S")
    except Exception:
        return ""


def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def save_json(path, obj):
    try:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(obj, f, ensure_ascii=False)
        os.replace(tmp, path)
    except Exception as e:
        print("保存 %s 失败: %s" % (path, e))


# 群消息形如 "wxid_xxx:\n正文"（自己发的则没有前缀）
_SENDER_RE = re.compile(r"^([A-Za-z0-9_@.\-]{4,64}):\n", re.S)

# 微信群消息表里，sender_id 是「群成员槽位」编号：
#   1    = 自己（实测两个群一致，且自己的消息 content 没有 "wxid_xxx:" 前缀）
#   6    = 系统消息（"xxx邀请你加入了群聊"）
#   其他 = 群成员
SELF_SENDER_ID = 1
SYSTEM_SENDER_ID = 6


def split_sender(raw, is_group, chat_id=""):
    """拆出 (sender_wxid, text)。

    - 群消息：形如 "wxid_xxx:\\n正文"；自己发的没有前缀
    - 私聊：正文里没有前缀，发送者默认就是会话对象本身
      （XML / emoji 消息能从 fromusername 解析出来，更准）
    """
    if is_group:
        m = _SENDER_RE.match(raw)
        if m:
            return m.group(1), raw[m.end():]
        return "", raw
    m2 = re.search(r'fromusername\s*=\s*"([^"]+)"', raw)
    if m2:
        return m2.group(1), raw
    return chat_id, raw


class ChatWatcher:
    """单个会话的增量监听（持有自己的水位）。"""

    def __init__(self, db, chat_id, my_wxid, args, cursor, names):
        self.db = db
        self.chat_id = chat_id
        self.my_wxid = my_wxid
        self.args = args
        self.cursor = cursor
        self.names = names
        self.is_group = chat_id.endswith("@chatroom")
        self.chat_name = db.get_nickname(chat_id) or chat_id
        self.last_local_id = cursor.get(chat_id)
        self.need_baseline = self.last_local_id is None
        self.seen = set()

    def who(self, wxid):
        if not wxid:
            return ""
        if wxid == self.my_wxid:
            return "我"
        # 固定昵称优先：老板要求叫他"喵"，不能是微信原始昵称 quantum/wakuwakumiao。
        # 注意必须放在缓存查询之前，否则会被 names.json 里的旧值盖掉。
        fixed = self.names.get("_fixed") or {}
        if isinstance(fixed, dict) and fixed.get(wxid):
            return fixed[wxid]
        if self.names.get(wxid):
            return self.names[wxid]
        n = self.db.get_nickname(wxid) or ""
        self.names[wxid] = n
        return n or wxid

    def poll(self):
        """读取一次；返回新记录列表（已写入队列）。"""
        msgs = self.db.get_messages(self.chat_id, limit=self.args.limit)
        msgs = sorted(msgs, key=lambda m: (m.get("local_id") or 0))

        if self.need_baseline:
            if msgs:
                self.last_local_id = msgs[-1].get("local_id")
                self.cursor[self.chat_id] = self.last_local_id
                save_json(self.args.cursor, self.cursor)
                print("[%s] %s 建立起始水位 = %s" % (now_iso(), self.chat_name, self.last_local_id))
            self.need_baseline = False
            return []

        new_records = []
        for m in msgs:
            lid = m.get("local_id")
            if lid is None:
                continue
            if self.last_local_id is not None and lid <= self.last_local_id:
                continue
            if lid in self.seen:
                continue
            self.seen.add(lid)

            raw = m.get("content") or ""
            sender_wxid, text = split_sender(raw, self.is_group, self.chat_id)
            if not sender_wxid:
                sender_wxid = m.get("sender_username") or ""

            # 判定「是不是自己发的」——多条独立依据，任一命中即可：
            #   1) content 解析出了自己的 wxid
            # 判定「是不是自己发的」——多条独立依据，任一命中即可：
            #   1) content 解析出了自己的 wxid
            #   2) sender_id == 1（消息表槽位 1 恒为自己，群聊/私聊都成立）
            # 注意：私聊时 sender_wxid 默认被填成会话对象（对方），
            #      所以【不能】先信 sender_wxid。sender_id 才是硬依据。
            sender_id = m.get("sender_id")
            is_self = bool(sender_wxid and sender_wxid == self.my_wxid)
            if sender_id == SELF_SENDER_ID:
                is_self = True
            is_system = self.is_group and sender_id == SYSTEM_SENDER_ID
            if is_self:
                sender_wxid = self.my_wxid
            elif self.is_group and not sender_wxid:
                sender_wxid = ""

            new_records.append({
                "at": now_iso(),
                "msgTime": fmt_time(m.get("create_time")),
                "platform": "wechat",
                "chatId": self.chat_id,
                "chatName": self.chat_name,
                "isGroup": self.is_group,
                "localId": lid,
                "senderId": sender_id,
                "senderWxid": sender_wxid,
                "senderName": "我" if is_self else self.who(sender_wxid),
                "type": m.get("type"),
                "text": text.strip(),
                "isSelf": is_self,
                "isSystem": is_system,
            })

        if new_records:
            with open(self.args.out, "a", encoding="utf-8") as f:
                for r in new_records:
                    f.write(json.dumps(r, ensure_ascii=False) + "\n")
            self.last_local_id = new_records[-1]["localId"]
            self.cursor[self.chat_id] = self.last_local_id
            save_json(self.args.cursor, self.cursor)
            save_json(self.args.names, self.names)
            for r in new_records:
                tag = r["senderName"] or r["senderWxid"] or "?"
                print("[%s] %s | %s: %s" % (r["msgTime"], r["type"], tag, (r["text"] or "[非文本]")[:70]))
        return new_records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chat", nargs="*", default=[], help="一个或多个会话 id（@chatroom 结尾是群）")
    ap.add_argument("--out", default="queue.jsonl")
    ap.add_argument("--cursor", default="cursor.json")
    ap.add_argument("--names", default="names.json", help="wxid -> 昵称缓存")
    ap.add_argument("--interval", type=float, default=1.5)
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    db = WeChatDB()
    me = db.get_self_info()
    my_wxid = me.get("username", "")
    print("[%s] 账号=%s 昵称=%s" % (now_iso(), my_wxid, me.get("nick_name", "")))

    if args.list:
        for s in db.get_sessions(limit=200):
            u = s.get("username", "")
            if u.endswith("@chatroom"):
                print("  %-30s %s" % ((db.get_nickname(u) or u)[:30], u))
        return

    # 支持 --chat a b c 和 --chat a,b,c 两种写法
    chats = []
    for c in args.chat:
        chats.extend([x for x in re.split(r"[,\s]+", c) if x])
    if not chats:
        print("需要 --chat 或 --list")
        sys.exit(1)

    cursor = load_json(args.cursor, {})
    names = load_json(args.names, {})

    # ── 固定昵称表（不改微信备注，只改桥接侧显示）──
    # 老板明确要求叫他"喵"（不是 quantum / wakuwakumiao）。
    # 存成 names["_fixed"]，who() 会最先查它，所以不会被缓存或订阅号昵称覆盖。
    fixed = names.get("_fixed")
    if not isinstance(fixed, dict):
        fixed = {}
    names["_fixed"] = fixed
    print("[%s] 固定昵称: %s" % (now_iso(), ", ".join("%s→%s" % (k, v) for k, v in fixed.items())))

    watchers = [ChatWatcher(db, cid, my_wxid, args, cursor, names) for cid in chats]

    print("[%s] 监听 %d 个会话:" % (now_iso(), len(watchers)))
    for w in watchers:
        print("    %s (%s)  水位=%s" % (w.chat_name, w.chat_id, w.last_local_id))
    print("[%s] 队列: %s" % (now_iso(), os.path.abspath(args.out)))
    print("[%s] 只读模式 —— 不会发送任何消息" % now_iso())
    print("")

    while True:
        for w in watchers:
            try:
                w.poll()
            except Exception as e:
                print("[%s] %s 读取异常: %s: %s" % (now_iso(), w.chat_name, type(e).__name__, e))

        if args.once:
            print("[%s] --once：退出" % now_iso())
            return
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
