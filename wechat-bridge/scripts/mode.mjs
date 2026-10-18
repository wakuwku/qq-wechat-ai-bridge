#!/usr/bin/env node
/**
 * mode.mjs —— 群指令：安静点 / 活跃点（移植自 qq-bridge 的 quiet-mode 思路）
 *
 * 与 QQ 的差异：
 *   QQ 走 /api/socialV2/config 改唤醒配置（因为它有潜水/唤醒机制）；
 *   微信这边没有"唤醒"，对应的是「选择性回复的沉默概率」和「发送频率」，
 *   所以改的是这两个参数，并且是**热生效**（闭环每轮读一次）。
 *
 * 权限：只有主人（ownerWxid）能改。别人说"安静点"只会被礼貌敷衍，不会真的改。
 */

import fs from 'node:fs';
import path from 'node:path';

export const MODES = {
  // 活跃：多接话，对话窗口长
  active: {
    label: '活跃',
    skipProbability: 0.25,
    recentReplyWindowMs: 180000,   // 3 分钟：聊起来就一直在
    maxPerMinute: 6,
    minGapMs: 5000,
  },
  // 正常：用 config.json 里的默认值（null = 不覆盖）
  normal: {
    label: '正常',
    skipProbability: null,
    recentReplyWindowMs: null,
    maxPerMinute: null,
    minGapMs: null,
  },
  // 安静：**少主动插话**，但不等于不理人。
  //   关键：沉默概率抬高只影响「没叫你、也不是对话延续」的闲聊；
  //   被点名/提问/提到自己 → isMustReply 直接放行；
  //   刚回完话的窗口内 → 也会继续回（保证对话不中断）。
  //   所以这里把窗口拉长（而不是缩短），跟 silent 相反。
  quiet: {
    label: '安静',
    skipProbability: 0.8,
    recentReplyWindowMs: 240000,   // 4 分钟：点名之后能持续聊
    maxPerMinute: 3,
    minGapMs: 20000,
  },
  // 闭嘴：极端档，基本只应点名，且对话窗口很短
  silent: {
    label: '闭嘴',
    skipProbability: 0.95,
    recentReplyWindowMs: 30000,
    maxPerMinute: 1,
    minGapMs: 120000,
  },
};

// 指令识别。NOTE: 故意写得宽松——群里说话随意，太严格会漏。
const CMD_PATTERNS = [
  { mode: 'silent', re: /(闭嘴|别说话|安静如鸡|shut\s*up|静音)/i },
  { mode: 'quiet', re: /(安静点|安静一下|小点声|少说点|少说两句|别刷屏|潜水|安静)/i },
  { mode: 'active', re: /(活跃点|活跃起来|出来聊|多说点|热闹点|别潜水|可以说话了)/i },
  { mode: 'normal', re: /(恢复正常|正常点|回到正常|正常模式)/i },
];
const STATUS_RE = /(什么模式|现在的模式|模式状态|你怎么不说话|你哑了)/i;

export class ModeManager {
  /**
   * @param {string} stateFile  持久化文件（state/mode.json）
   * @param {string} ownerWxid  主人 wxid（只有他能改）
   * @param {object} baseCfg    config.json 里的 social / rateLimit 原始值
   */
  constructor(stateFile, ownerWxid, baseCfg = {}) {
    this.stateFile = stateFile;
    this.ownerWxid = ownerWxid;
    this.base = baseCfg;
    this.state = { mode: 'normal', setBy: '', setAt: '', reason: '' };
    this.load();
  }

  load() {
    try {
      const raw = JSON.parse(fs.readFileSync(this.stateFile, 'utf8'));
      if (raw && MODES[raw.mode]) this.state = { ...this.state, ...raw };
    } catch { /* 首次运行没有文件 */ }
    return this;
  }

  save() {
    try {
      fs.mkdirSync(path.dirname(this.stateFile), { recursive: true });
      fs.writeFileSync(this.stateFile, JSON.stringify(this.state, null, 2), 'utf8');
    } catch { /* 失败不影响主流程 */ }
  }

  get mode() { return this.state.mode; }
  get modeInfo() { return MODES[this.state.mode] ?? MODES.normal; }

  /** 群指令入口。返回 { handled, reply, changed } */
  handle(text, senderWxid, senderName = '') {
    const t = String(text ?? '');
    const isOwner = senderWxid && senderWxid === this.ownerWxid;

    // 状态查询：谁都能问
    if (STATUS_RE.test(t)) {
      const info = this.modeInfo;
      return {
        handled: true,
        changed: false,
        reply: `现在是「${info.label}」模式${isOwner ? '' : '（只有喵能改）'}`,
      };
    }

    for (const { mode, re } of CMD_PATTERNS) {
      if (!re.test(t)) continue;

      if (!isOwner) {
        // 非主人：不报错、不解释权限系统，就用角色口吻敷衍过去
        return {
          handled: true,
          changed: false,
          reply: '这得喵说了才算',
        };
      }

      if (this.state.mode === mode) {
        return { handled: true, changed: false, reply: `已经是「${MODES[mode].label}」了` };
      }

      this.state = {
        mode,
        setBy: senderWxid,
        setAt: new Date().toISOString(),
        reason: t.slice(0, 60),
      };
      this.save();

      const ack = {
        active: '好，那我出来了',
        quiet: '行，我少说两句（你叫我还是会应的）',
        silent: '……',
        normal: '好，正常来了',
      }[mode] ?? '好';
      return { handled: true, changed: true, reply: ack };
    }

    return { handled: false, changed: false, reply: '' };
  }

  /**
   * 把当前模式应用到 social / rateLimit 配置。
   * normal 模式返回 null（表示用 config.json 原始值）。
   */
  applyTo() {
    const info = this.modeInfo;
    if (this.state.mode === 'normal') return null;
    const out = {};
    if (info.skipProbability != null) out.skipProbability = info.skipProbability;
    if (info.recentReplyWindowMs != null) out.recentReplyWindowMs = info.recentReplyWindowMs;
    if (info.maxPerMinute != null) out.maxPerMinute = info.maxPerMinute;
    if (info.minGapMs != null) out.minGapMs = info.minGapMs;
    return out;
  }

  describe() {
    const info = this.modeInfo;
    return {
      mode: this.state.mode,
      label: info.label,
      setBy: this.state.setBy,
      setAt: this.state.setAt,
      overrides: this.applyTo(),
    };
  }
}

export default ModeManager;
