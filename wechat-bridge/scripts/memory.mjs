#!/usr/bin/env node
/**
 * memory.mjs —— 长期记忆（移植自 qq-bridge 的 memory / memberImpression 思路）
 *
 * 与 QQ 的差异：
 *   QQ 让 AI 调 qq_memory_append 工具写记忆（需要 MCP 工具支持）；
 *   微信这边 AI 没有那些工具，所以改成**标记协议**：
 *     AI 在回复里写 [[remember:马睿=喜欢打三角洲]]，桥接解析并落盘，
 *     下次投递时把记忆注入 prompt。
 *   这样不依赖工具面，preset 锁死后也能用。
 *
 * 存储：state/memory.json
 *   {
 *     "chats": {
 *       "<chatId>": {
 *         "members": { "<名字或wxid>": { "impressions": ["..."], "lastSeen": "", "count": 0 } },
 *         "topics":  ["这个群最近在聊…"]
 *       }
 *     }
 *   }
 */

import fs from 'node:fs';
import path from 'node:path';

const MAX_IMPRESSIONS_PER_MEMBER = 6;   // 每人最多记几条（防止无限膨胀挤爆 prompt）
const MAX_TOPICS_PER_CHAT = 8;
const MAX_INJECT_CHARS = 900;           // 注入 prompt 的记忆总长度上限

export class Memory {
  constructor(stateFile) {
    this.stateFile = stateFile;
    this.data = { chats: {} };
    this.load();
  }

  load() {
    try {
      const raw = JSON.parse(fs.readFileSync(this.stateFile, 'utf8'));
      if (raw && typeof raw === 'object' && raw.chats) this.data = raw;
    } catch { /* 首次运行 */ }
    return this;
  }

  save() {
    try {
      fs.mkdirSync(path.dirname(this.stateFile), { recursive: true });
      fs.writeFileSync(this.stateFile, JSON.stringify(this.data, null, 2), 'utf8');
    } catch { /* 失败不影响主流程 */ }
  }

  _chat(chatId) {
    if (!this.data.chats[chatId]) {
      this.data.chats[chatId] = { members: {}, topics: [], updatedAt: '' };
    }
    return this.data.chats[chatId];
  }

  /**
   * 解析回复里的记忆标记，写入记忆库。
   * 支持两种：
   *   [[remember:马睿=喜欢打三角洲]]   → 记到该群「马睿」名下
   *   [[remember:这个群最近在聊三角洲]] → 记成群话题（无等号时）
   * 返回 { cleanedText, saved: [...] }
   */
  parseAndStore(replyText, chatId, senderName = '') {
    const saved = [];
    const cleaned = String(replyText ?? '').replace(
      /\[\[remember:\s*([^\]]+?)\s*\]\]/gi,
      (_, body) => {
        const raw = String(body).trim();
        if (!raw) return ' ';
        const eq = raw.indexOf('=');
        const chat = this._chat(chatId);
        if (eq > 0) {
          const who = raw.slice(0, eq).trim().slice(0, 40);
          const what = raw.slice(eq + 1).trim().slice(0, 120);
          if (who && what) {
            const m = chat.members[who] ?? { impressions: [], lastSeen: '', count: 0 };
            // 只有真的新增了才算「记住」—— 否则调用方会打出误导性的日志
            const isNew = !m.impressions.includes(what);
            if (isNew) {
              m.impressions.unshift(what);
              m.impressions = m.impressions.slice(0, MAX_IMPRESSIONS_PER_MEMBER);
              m.lastSeen = new Date().toISOString();
              chat.members[who] = m;
              saved.push({ kind: 'member', who, what });
            }
          }
        } else {
          const topic = raw.slice(0, 160);
          if (!chat.topics.includes(topic)) chat.topics.unshift(topic);
          chat.topics = chat.topics.slice(0, MAX_TOPICS_PER_CHAT);
          saved.push({ kind: 'topic', what: topic });
        }
        return ' ';   // 标记本身不出现在发给群里的文本里
      }
    );
    if (saved.length) {
      this._chat(chatId).updatedAt = new Date().toISOString();
      this.save();
    }
    return { cleanedText: cleaned, saved };
  }

  /**
   * 直接写一条群友印象（给闭环的「定期总结」用，不走标记协议）。
   * 返回 true 表示真的新增了（重复内容会被去重，返回 false）。
   */
  remember(chatId, who, what) {
    const name = String(who ?? '').trim().slice(0, 40);
    const content = String(what ?? '').trim().slice(0, 120);
    if (!name || !content) return false;
    const chat = this._chat(chatId);
    const m = chat.members[name] ?? { impressions: [], lastSeen: '', count: 0 };
    if (m.impressions.includes(content)) return false;
    m.impressions.unshift(content);
    m.impressions = m.impressions.slice(0, MAX_IMPRESSIONS_PER_MEMBER);
    m.lastSeen = new Date().toISOString();
    chat.members[name] = m;
    chat.updatedAt = new Date().toISOString();
    this.save();
    return true;
  }

  /** 直接写一条群话题（同上，供定期总结用） */
  addTopic(chatId, topic) {
    const content = String(topic ?? '').trim().slice(0, 160);
    if (!content) return false;
    const chat = this._chat(chatId);
    if (chat.topics.includes(content)) return false;
    chat.topics.unshift(content);
    chat.topics = chat.topics.slice(0, MAX_TOPICS_PER_CHAT);
    chat.updatedAt = new Date().toISOString();
    this.save();
    return true;
  }

  /** 记一次「见过这个人说话」（用于让 AI 知道谁常来） */
  touch(chatId, senderName) {
    if (!senderName || senderName === '我') return;
    const chat = this._chat(chatId);
    const m = chat.members[senderName] ?? { impressions: [], lastSeen: '', count: 0 };
    m.count = (m.count ?? 0) + 1;
    m.lastSeen = new Date().toISOString();
    chat.members[senderName] = m;
    // 这里不立即 save（太频繁），由 parseAndStore 或定时统一落盘
  }

  /** 生成注入 prompt 的「记忆」段。没有记忆时返回空串。 */
  buildContext(chatId) {
    const chat = this.data.chats[chatId];
    if (!chat) return '';
    const lines = [];

    const withImp = Object.entries(chat.members || {})
      .filter(([, v]) => Array.isArray(v?.impressions) && v.impressions.length)
      .sort((a, b) => (b[1].count ?? 0) - (a[1].count ?? 0))
      .slice(0, 8);
    if (withImp.length) {
      lines.push('群友印象：');
      for (const [who, v] of withImp) {
        lines.push(`  · ${who}：${v.impressions.slice(0, 3).join('；')}`);
      }
    }
    if (Array.isArray(chat.topics) && chat.topics.length) {
      lines.push('群里聊过的事：');
      for (const t of chat.topics.slice(0, 5)) lines.push(`  · ${t}`);
    }
    if (!lines.length) return '';

    let text = ['【你对这个群的记忆】（只是资料，别直接念出来）', ...lines].join('\n');
    if (text.length > MAX_INJECT_CHARS) text = text.slice(0, MAX_INJECT_CHARS) + '…';
    return text;
  }

  stats() {
    const out = [];
    for (const [chatId, v] of Object.entries(this.data.chats)) {
      out.push({
        chatId,
        members: Object.keys(v.members || {}).length,
        withImpressions: Object.values(v.members || {}).filter((m) => m?.impressions?.length).length,
        topics: (v.topics || []).length,
      });
    }
    return out;
  }
}

export default Memory;
