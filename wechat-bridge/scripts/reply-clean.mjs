#!/usr/bin/env node
/**
 * reply-clean.mjs —— 挡住「模型把思考过程写进正文发到群里」
 *
 * 真实事故（2026-10-17 微信「神人微信分号」）：
 *   发进群的那条消息是：
 *     "Wait, that id isn't in the list. Use 9694a90f. Just output sticker or a short line.
 *      Keep it simple. 喵又发表情包了……那我回个轻松点的。用 00a96eda 投降喵 fits ("喵").
 *      Actually the sender is 喵, 投降喵 works well.投降喵，你表情包比话还多"
 *
 *   会话记录里**没有 reasoning 块** —— 模型真的把内心独白写进了正文 text 块，
 *   而 loop 的收集器只拼 text 块，于是整段被当成回复发了出去。
 *   同一段污染还带出了一个抄错的表情 id（9694a90f → 69d4a90f）。
 *
 * 策略：**只在多行时才清洗**。提示词本来就硬性要求"单行一条"，
 * 所以出现多行本身就说明模型违约了；单行回复一律原样返回，避免误伤。
 */
const DELIBERATION_PATTERNS = [
  /[A-Za-z]{2,}(?:[\s,.'"-]+[A-Za-z']{2,}){2,}/,                        // 连续 3 个以上英文词
  /\b[0-9a-f]{8,32}\b/i,                                                 // 裸的表情 id
  /\b(let me|keep it simple|should i|i should|just output|isn'?t|actually|wait[,.])\b/i,
];

export function looksLikeDeliberation(line) {
  return DELIBERATION_PATTERNS.some((re) => re.test(String(line ?? '')));
}

/**
 * @param {string} raw  模型原始回复
 * @param {(msg:string)=>void} [log]
 * @returns {string} 要发出去的那一行
 */
export function cleanReply(raw, log = () => {}) {
  const text = String(raw ?? '');
  const lines = text.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
  if (lines.length <= 1) return text.trim();
  const kept = lines.filter((line) => !looksLikeDeliberation(line));
  if (!kept.length) {
    log(`⚠️ ${lines.length} 行全都像思考过程 —— 取最后一行兜底`);
    return lines[lines.length - 1];
  }
  if (kept.length !== lines.length) {
    log(`（清掉 ${lines.length - kept.length} 行像思考过程的内容，只保留 1 行）`);
  }
  return kept[kept.length - 1];
}
