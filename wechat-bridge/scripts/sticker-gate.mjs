#!/usr/bin/env node
/**
 * sticker-gate.mjs —— 表情包的发送频率闸门
 *
 * 问题：模型被提示词惯成了"每条回复都塞一个表情"，群里看着像在打广告。
 * 光改提示词不可靠（模型未必听话），所以这里做**机械闸门**：
 * 模型给不给是它的事，最终发不发由概率决定，平均约每 3 条回复带 1 次。
 *
 * 三个形态都要能出现：
 *   · 不发   —— 大多数时候
 *   · 发一个 —— 常态
 *   · 连发   —— 模型一次给了多个时，按 burstProbability 决定要不要都发
 *
 * 纯函数 + 可注入 random，便于离线验证分布。
 */
function clampUnit(value, fallback) {
  const number = Number(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.min(1, Math.max(0, number));
}

/**
 * @param {string[]} ids 模型想发的表情 id（可能有多个）
 * @param {{probability?:number,maxPerReply?:number,burstProbability?:number}} config
 * @param {() => number} random
 * @returns {{chosen: string[], reason: 'no_candidate'|'gated_out'|'single'|'burst'}}
 */
export function gateStickers(ids, config = {}, random = Math.random) {
  const list = Array.isArray(ids) ? ids.map(String).filter(Boolean) : [];
  if (!list.length) return { chosen: [], reason: 'no_candidate' };

  const probability = clampUnit(config.probability, 0.34);
  if (random() >= probability) return { chosen: [], reason: 'gated_out' };

  const max = Math.max(1, Math.min(3, Math.trunc(Number(config.maxPerReply)) || 2));
  let chosen = list.slice(0, max);
  if (chosen.length > 1) {
    const burst = clampUnit(config.burstProbability, 0.35);
    if (random() >= burst) chosen = chosen.slice(0, 1);
  }
  return { chosen, reason: chosen.length > 1 ? 'burst' : 'single' };
}
