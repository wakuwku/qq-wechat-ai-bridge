#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
group_roster.py —— 生成微信群的「群友名单」，供注入到会话人格里

目的：让大肥鱼知道每个群里都有谁（轻量版群友认知）。
     不带长期记忆、不带印象累积，只是"名单"。

用法:
  python group_roster.py --chat 12345678901@chatroom           # 打印名单
  python group_roster.py --chat 12345678901@chatroom --json    # 输出 JSON
  python group_roster.py --all                                 # 所有群

输出 JSON（供 loop.mjs 调用）：
  {"chatId": "...", "chatName": "...", "members": [{"wxid":"...","name":"..."}]}
"""
import argparse
import json
import sys

from wechatauto import WeChatDB

# 固定昵称表（与 watch_wechat.py 保持一致，不改微信备注，只改桥接侧显示）
FIXED_NAMES = {
}


def roster_for(db, chat_id, my_wxid, my_name):
    chat_name = db.get_nickname(chat_id) or chat_id
    members = []
    try:
        raw = db.get_group_members(chat_id) or []
    except Exception as e:
        print("读群成员失败 %s: %s" % (chat_id, e), file=sys.stderr)
        raw = []

    seen = set()
    for m in raw:
        wxid = (m.get("username") or "").strip()
        if not wxid or wxid in seen:
            continue
        seen.add(wxid)

        if wxid == my_wxid:
            name = my_name or "大肥鱼"
            role = "你自己"
        elif wxid in FIXED_NAMES:
            name = FIXED_NAMES[wxid]
            role = "你的主人"
        else:
            # 群里显示名优先（nick_name），退到备注，再退 remark
            name = (m.get("nick_name") or "").strip() or (m.get("remark") or "").strip() or wxid
            role = "群主" if str(m.get("is_owner")).lower() == "true" else ""

        members.append({"wxid": wxid, "name": name, "role": role})

    # 主人和自己排前面，其余按名字排
    def sort_key(x):
        order = {"你自己": 0, "你的主人": 1, "群主": 2}.get(x["role"], 3)
        return (order, x["name"])
    members.sort(key=sort_key)

    return {"chatId": chat_id, "chatName": chat_name, "members": members}


def render_markdown(roster):
    """渲染成注入用 markdown"""
    lines = ["", "---", "", "# 【你现在所在的群：%s】" % roster["chatName"], ""]
    lines.append("群 ID：`%s`" % roster["chatId"])
    lines.append("")
    lines.append("**这个群里的人（你全都认识，能对上号）：**")
    lines.append("")
    for m in roster["members"]:
        tag = ""
        if m["role"] == "你自己":
            tag = "  ← **这就是你**"
        elif m["role"] == "你的主人":
            tag = "  ← **你的主人，叫他「喵」**"
        elif m["role"] == "群主":
            tag = "  ← 群主"
        lines.append("- **%s**%s" % (m["name"], tag))
    lines.append("")
    lines.append("**用法（重要，别说错）：**")
    lines.append("- 你**正在**「%s」这个群里说话，上面就是本群的人" % roster["chatName"])
    lines.append("- 有人提到上面任何一个名字 → 你**认识**，别说不认识、别说「没这号人」")
    lines.append("- 别人问「群里都有谁」→ 顺口报几个，别背课文、别念编号")
    lines.append("- 名单里**没有**的人 → 才是没见过的，正常搭话就行")
    lines.append("- 你同时在好几个群里，**别把其他群的人当成本群的**")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chat", help="群 id")
    ap.add_argument("--all", action="store_true", help="所有群")
    ap.add_argument("--json", action="store_true", help="输出 JSON（给 loop.mjs 用）")
    ap.add_argument("--markdown", action="store_true", help="输出可注入的 markdown")
    args = ap.parse_args()

    db = WeChatDB()
    me = db.get_self_info()
    my_wxid = me.get("username", "")
    my_name = me.get("nick_name") or "大肥鱼"

    if args.all:
        chats = [s.get("username") for s in db.get_sessions(limit=200)
                 if str(s.get("username", "")).endswith("@chatroom")]
    elif args.chat:
        chats = [args.chat]
    else:
        print("需要 --chat 或 --all")
        return 2

    rosters = [roster_for(db, c, my_wxid, my_name) for c in chats]

    if args.json:
        print(json.dumps(rosters[0] if len(rosters) == 1 else rosters, ensure_ascii=False))
    elif args.markdown:
        for r in rosters:
            print(render_markdown(r))
    else:
        for r in rosters:
            print("群: %s (%s)  共 %d 人" % (r["chatName"], r["chatId"], len(r["members"])))
            for m in r["members"]:
                extra = (" [%s]" % m["role"]) if m["role"] else ""
                print("    %-20s %s%s" % (m["name"][:20], m["wxid"], extra))
            print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
