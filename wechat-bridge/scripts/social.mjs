#!/usr/bin/env node
/**
 * social.mjs —— 选择性回复 / 社交状态机（移植自 qq-bridge）
 *
 * 目的：让大肥鱼不像个"逢消息必回"的机器人，而是像真人一样
 *       ——大部分普通闲聊会看到但不接，被点名/提问才必回。
 *
 * 参数与 qq-bridge 的 cfg.social 对齐，方便两边行为一致。
 *
 * 核心规则（照搬 QQ 的做法）：
 *   1. 必回：被 @ / 被点名 / 有人引用我 / 直接提问 / 私聊
 *   2. 刚回过话（60s 内）→ 不沉默，保证对话连贯
 *   3. 沉默概率随"待回消息堆积"递减（消息越多越该回）
 *   4. 沉默掉的消息不丢，攒进 silentContext，下次回复时一并带给模型
 *      （真人视角：我看到过，只是当时没接）
 */
import fs from 'node:fs';
import path from 'node:path';

const DEFAULTS = {
  skipProbability: 0.35,        // 基准沉默概率（普通闲聊）
  recentReplyWindowMs: 60_000,  // 刚回过话的窗口：窗口内不沉默
  pressurePerMessage: 0.1,      // 每条堆积消息降低的沉默概率
  pressureCap: 0.5,             // 降低上限
  silentContextMax: 30,         // 攒着的沉默消息上限
  // 触发"进入对话"的概率（目前微信是每条都投递，这个值先留着备用）
  triggerProbability: 0.1,
};

/** 判断这条消息是不是"必须回" */
export function isMustReply(rec, opts = {}) {
  const text = String(rec.text ?? '');
  if (rec.isGroup === false) return true;                    // 私聊必回
  if (opts.mentioned) return true;                           // 被 @
  if (opts.quoted) return true;                              // 被引用
  // 直接提问（问号、疑问词）
  if (/[?？]/.test(text)) return true;
  if (/(在吗|在不在|咋办|怎么办|为什么|为啥|是不是|能不能|可不可以|帮我看|帮我查)/.test(text)) return true;
  // 被叫名字（大肥鱼 / 小鲸鱼 / 鱼）
  if (/(大肥鱼|小鲸鱼|肥鱼)/.test(text)) return true;
  return false;
}

export class SocialState {
  /**
   * @param {object} cfg  { skipProbability, recentReplyWindowMs, ... }
   * @param {string} stateFile  持久化文件（可选）
   */
  constructor(cfg = {}, stateFile = '') {
    this.cfg = { ...DEFAULTS, ...(cfg || {}) };
    this.stateFile = stateFile;
    this.chats = new Map(); // chatId -> { lastAiReplyAt, silentContext: [] }
    this._load();
  }

  _load() {
    if (!this.stateFile) return;
    try {
      const raw = JSON.parse(fs.readFileSync(this.stateFile, 'utf8'));
      for (const [k, v] of Object.entries(raw.chats ?? {})) {
        this.chats.set(k, {
          lastAiReplyAt: v.lastAiReplyAt ?? 0,
          silentContext: Array.isArray(v.silentContext) ? v.silentContext : [],
        });
      }
    } catch { /* 首次运行没有文件，正常 */ }
  }

  _save() {
    if (!this.stateFile) return;
    try {
      const obj = { chats: {} };
      for (const [k, v] of this.chats) obj.chats[k] = v;
      fs.mkdirSync(path.dirname(this.stateFile), { recursive: true });
      fs.writeFileSync(this.stateFile, JSON.stringify(obj, null, 2));
    } catch { /* 保存失败不影响主流程 */ }
  }

  _st(chatId) {
    if (!this.chats.has(chatId)) {
      this.chats.set(chatId, { lastAiReplyAt: 0, silentContext: [] });
    }
    return this.chats.get(chatId);
  }

  /**
   * 决定这条消息要不要回。
   * @returns {{reply:boolean, reason:string, skippedCount:number, pendingContext:Array}}
   */
  decide(rec, opts = {}) {
    const st = this._st(rec.chatId);
    const now = Date.now();
    const silentCount = st.silentContext.length;

    // 1) 必回
    if (isMustReply(rec, opts)) {
      return {
        reply: true,
        reason: opts.mentioned ? '被@' : (opts.quoted ? '被引用' : '提问/点名'),
        skippedCount: silentCount,
        pendingContext: st.silentContext.slice(),
      };
    }

    // 2) 刚回过话 → 不沉默（对话连贯性）
    const sinceAiReply = now - (st.lastAiReplyAt || 0);
    if (st.lastAiReplyAt && sinceAiReply < this.cfg.recentReplyWindowMs) {
      return { reply: true, reason: `刚回过话(${Math.round(sinceAiReply / 1000)}s前)`, skippedCount: silentCount, pendingContext: st.silentContext.slice() };
    }

    // 3) 计算沉默概率（消息堆积会降低它）
    let skipProb = Math.min(1, Math.max(0, Number(this.cfg.skipProbability)));
    const pressure = Math.min(this.cfg.pressureCap, silentCount * this.cfg.pressurePerMessage);
    skipProb = Math.max(0, skipProb - pressure);

    if (Math.random() < skipProb) {
      // 沉默：把消息攒起来，下次一起给模型
      st.silentContext.push({ sender: rec.senderName, text: rec.text, time: rec.msgTime });
      if (st.silentContext.length > this.cfg.silentContextMax) {
        st.silentContext = st.silentContext.slice(-this.cfg.silentContextMax);
      }
      this._save();
      return { reply: false, reason: `选择性沉默(skip=${skipProb.toFixed(2)})`, skippedCount: silentCount + 1, pendingContext: [] };
    }

    return { reply: true, reason: `未触发沉默(skip=${skipProb.toFixed(2)})`, skippedCount: silentCount, pendingContext: st.silentContext.slice() };
  }

  /** 记一次"我说话了"（并清空沉默缓存——那些消息已经随本次回复带过去了） */
  markReplied(chatId) {
    const st = this._st(chatId);
    st.lastAiReplyAt = Date.now();
    st.silentContext = [];
    this._save();
  }

  /** 供 prompt 用：把沉默期间的消息渲染成一段文字 */
  static renderPending(pending) {
    if (!pending?.length) return '';
    const lines = pending.map((m) => `  · ${m.sender}: ${m.text}`);
    return ['（下面这些是你刚才看到、但没接的话）', ...lines].join('\n');
  }

  stats() {
    const out = [];
    for (const [k, v] of this.chats) {
      out.push({ chatId: k, lastAiReplyAt: v.lastAiReplyAt, pending: v.silentContext.length });
    }
    return out;
  }
}

export default SocialState;
