/**
 * test-mode-memory.mjs —— mode.mjs / memory.mjs 的单元测试
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ModeManager } from './mode.mjs';
import { Memory } from './memory.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'wx-mm-'));
const OWNER = 'wxid_owner';
const OTHER = 'wxid_other';

let pass = 0, fail = 0;
function check(name, cond, extra = '') {
  if (cond) { console.log(`  ✅ ${name}`); pass++; }
  else { console.log(`  ❌ ${name} ${extra}`); fail++; }
}

console.log('=== mode.mjs（群指令）===');
{
  const m = new ModeManager(path.join(TMP, 'mode.json'), OWNER);
  check('默认模式 = normal', m.mode === 'normal');

  // owner 说"安静点"
  let r = m.handle('@大肥鱼 安静点', OWNER, '喵');
  check('owner 说安静点 → handled', r.handled);
  check('owner 说安静点 → changed', r.changed);
  check('模式变成 quiet', m.mode === 'quiet', `实际=${m.mode}`);
  check('有回复文案', !!r.reply, r.reply);
  const ov = m.applyTo();
  check('quiet 覆盖了沉默概率(0.8)', ov && ov.skipProbability === 0.8, JSON.stringify(ov));
  check('quiet 的【回话窗口被拉长】- 点名后能持续聊', ov && ov.recentReplyWindowMs === 240000, JSON.stringify(ov));
  check('quiet 窗口 > normal 窗口(90s)', ov && ov.recentReplyWindowMs > 90000, JSON.stringify(ov));

  // 重复指令
  r = m.handle('安静点', OWNER);
  check('重复说安静点 → 不再 changed', r.handled && !r.changed, JSON.stringify(r));

  // 别人说
  r = m.handle('活跃点', OTHER, '路人');
  check('别人说活跃点 → 不改变模式', m.mode === 'quiet', `实际=${m.mode}`);
  check('别人说活跃点 → 有礼貌的拒绝文案', r.handled && !!r.reply, r.reply);

  // owner 切到「活跃」（这是新增的中间档之上的高档）
  r = m.handle('活跃点', OWNER);
  check('owner 说活跃点 → active 模式', m.mode === 'active' && r.changed, `实际=${m.mode}`);
  const ovA = m.applyTo();
  check('active 沉默概率 0.25', ovA?.skipProbability === 0.25, JSON.stringify(ovA));
  check('active 回话窗口 3 分钟', ovA?.recentReplyWindowMs === 180000, JSON.stringify(ovA));

  // 回到「正常」——这条专门防"切回 normal 后参数没恢复"的回归
  r = m.handle('恢复正常', OWNER);
  check('owner 说恢复正常 → normal 模式', m.mode === 'normal' && r.changed, `实际=${m.mode}`);
  check('normal 时 applyTo() 返回 null（表示用 config 原值）', m.applyTo() === null, JSON.stringify(m.applyTo()));

  // 闭嘴
  r = m.handle('闭嘴', OWNER);
  check('owner 说闭嘴 → silent', m.mode === 'silent');
  const ovS = m.applyTo();
  check('silent 的窗口很短(30s)', ovS?.recentReplyWindowMs === 30000, JSON.stringify(ovS));

  // 状态查询（谁都能问）
  r = m.handle('现在什么模式', OTHER);
  check('状态查询 → handled', r.handled);
  check('状态查询含模式名', /闭嘴|安静|正常/.test(r.reply), r.reply);

  // 非指令
  r = m.handle('今天天气不错', OWNER);
  check('普通闲聊 → 不拦截', !r.handled);

  // 持久化
  const m2 = new ModeManager(path.join(TMP, 'mode.json'), OWNER);
  check('模式已持久化', m2.mode === 'silent', `实际=${m2.mode}`);
}

console.log('');
console.log('=== memory.mjs（长期记忆）===');
{
  const mem = new Memory(path.join(TMP, 'memory.json'));
  const CHAT = '123@chatroom';

  check('初始无记忆 → buildContext 为空', mem.buildContext(CHAT) === '');

  // 记忆标记：带等号 → 群友印象
  let res = mem.parseAndStore('记住了 [[remember:马睿=喜欢打三角洲]] 好', CHAT, '马睿');
  check('解析出 1 条记忆', res.saved.length === 1);
  check('标记被清理出正文', !res.cleanedText.includes('[[remember'), res.cleanedText);
  check('正文保留其他内容', res.cleanedText.includes('记住了') && res.cleanedText.includes('好'), res.cleanedText);

  // 记忆标记：无等号 → 群话题
  res = mem.parseAndStore('[[remember:这个群最近在聊三角洲]]', CHAT, '喵');
  check('无等号 → 记成群话题', res.saved[0]?.kind === 'topic');

  // 注入
  const ctx = mem.buildContext(CHAT);
  check('注入包含群友印象', ctx.includes('马睿') && ctx.includes('三角洲'), ctx.slice(0, 80));
  check('注入包含群话题', ctx.includes('群里聊过的事'));
  check('注入有"别念出来"的框定', ctx.includes('别直接念出来'));

  // touch
  mem.touch(CHAT, '流雨');
  mem.touch(CHAT, '流雨');
  const before = mem.buildContext(CHAT);
  check('touch 不产生垃圾注入（无印象就不列）', !before.includes('流雨'), before.slice(0, 100));

  // 去重
  const n1 = mem.parseAndStore('[[remember:马睿=喜欢打三角洲]]', CHAT).saved.length;
  check('重复记忆不重复入库', n1 === 0, `saved=${n1}`);

  // 容量上限
  for (let i = 0; i < 12; i++) mem.parseAndStore(`[[remember:马睿=爱好${i}]]`, CHAT);
  const st = mem.stats().find((x) => x.chatId === CHAT);
  check('单人群友印象有条数上限', st.withImpressions === 1 && st.members >= 1, JSON.stringify(st));
  const member = JSON.parse(fs.readFileSync(path.join(TMP, 'memory.json'), 'utf8')).chats[CHAT].members['马睿'];
  check('印象数组不超过上限 6 条', member.impressions.length <= 6, `实际 ${member.impressions.length}`);

  // 别的群互不影响
  const ctx2 = mem.buildContext('other@chatroom');
  check('其他群没有串记忆', ctx2 === '', ctx2.slice(0, 60));
}

console.log('');
console.log(`结果: 通过 ${pass} / 失败 ${fail}`);
try { fs.rmSync(TMP, { recursive: true, force: true }); } catch {}
process.exit(fail ? 1 : 0);
