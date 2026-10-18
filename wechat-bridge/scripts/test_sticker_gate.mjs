#!/usr/bin/env node
/**
 * test_sticker_gate.mjs —— 表情闸门的离线验证
 *
 * 用确定性的伪随机序列验证「平均每 3 条回复带 1 次表情」这个目标频率，
 * 以及三个形态（不发/发一个/连发）都能出现。
 */
import { gateStickers } from './sticker-gate.mjs';

let checks = 0;
function check(ok, label, extra) {
  checks += 1;
  if (!ok) {
    console.error(`FAIL ${checks}: ${label}`, extra === undefined ? '' : `\n  got: ${extra}`);
    process.exit(1);
  }
}

/** 简单确定性 LCG，避免依赖 Math.random 导致测试不稳定 */
function lcg(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state * 1664525 + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

// 空候选
check(gateStickers([], {}, () => 0).chosen.length === 0, '没有候选时不发');
check(gateStickers(null, {}, () => 0).chosen.length === 0, 'null 候选不报错');

// 明确放行 / 明确拦下
check(gateStickers(['a'], { probability: 0.34 }, () => 0).chosen.length === 1, 'random=0 一定放行');
check(gateStickers(['a'], { probability: 0.34 }, () => 0.99).chosen.length === 0, 'random=0.99 一定拦下');
check(gateStickers(['a'], { probability: 0.34 }, () => 0.99).reason === 'gated_out', '拦下原因正确');

// 单候选：放行时就是 1 个，不会凭空变多
const single = gateStickers(['a'], { probability: 1, burstProbability: 1 }, () => 0);
check(single.chosen.length === 1, '单候选放行只发 1 个');

// 多候选受 maxPerReply 约束
const many = gateStickers(['a', 'b', 'c', 'd'], { probability: 1, maxPerReply: 2, burstProbability: 1 }, () => 0);
check(many.chosen.length === 2, 'maxPerReply=2 时最多 2 个', many.chosen.length);
check(many.reason === 'burst', '多候选判为连发', many.reason);

// 连发概率：burstProbability=0 时，多候选也只留 1 个
let rng = lcg(7);
const noBurst = gateStickers(['a', 'b'], { probability: 1, burstProbability: 0 }, () => 0);
check(noBurst.chosen.length === 1, 'burstProbability=0 时不连发', noBurst.chosen.length);

// 目标频率：模拟 20000 条回复，模型每条都给 1 个候选
rng = lcg(20261017);
const N = 20000;
let sent = 0;
for (let i = 0; i < N; i += 1) {
  if (gateStickers(['x'], { probability: 0.34 }, rng).chosen.length) sent += 1;
}
const rate = sent / N;
check(Math.abs(rate - 0.34) < 0.02, `平均频率应≈0.34（实测 ${rate.toFixed(4)}）`, rate);

// 三形态都要出现
rng = lcg(99);
const seen = new Set();
for (let i = 0; i < 4000; i += 1) {
  seen.add(gateStickers(['x', 'y'], { probability: 0.34, burstProbability: 0.35 }, rng).reason);
}
check(seen.has('gated_out'), '样本里出现「不发」', [...seen].join(','));
check(seen.has('single'), '样本里出现「发一个」', [...seen].join(','));
check(seen.has('burst'), '样本里出现「连发」', [...seen].join(','));

// 参数非法时的兜底
check(gateStickers(['a'], { probability: 'abc', maxPerReply: -1 }, () => 0).chosen.length === 1, '非法参数走默认值');
check(gateStickers(['a'], { probability: 2 }, () => 0.9).chosen.length === 1, 'probability>1 被夹到 1');

console.log(`PASS ${checks} assertions on sticker gate (rate=${rate.toFixed(4)})`);
