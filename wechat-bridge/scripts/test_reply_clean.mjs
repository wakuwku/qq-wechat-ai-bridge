#!/usr/bin/env node
/**
 * test_reply_clean.mjs —— 回复清洗的回归测试
 *
 * 第一个用例是**真实事故原文**（2026-10-17 微信「神人微信分号」实际发进群的那条），
 * 它当时把模型的英文内心独白和抄错的表情 id 一起发了出去。
 */
import { cleanReply } from './reply-clean.mjs';

let checks = 0;
function check(ok, label, extra) {
  checks += 1;
  if (!ok) {
    console.error(`FAIL ${checks}: ${label}`, extra === undefined ? '' : `\n  got: ${JSON.stringify(extra)}`);
    process.exit(1);
  }
}

const leaked = [
  'Wait, that id isn\'t in the list. Use 9694a90f.',
  'Just output sticker or a short line. Keep it simple.',
  '喵又发表情包了……那我回个轻松点的。用 00a96eda 投降喵 fits ("喵").',
  'Actually the sender is 喵, 投降喵 works well.',
  '投降喵，你表情包比话还多 [[sticker:00a96eda]]',
].join('\n');

let got = cleanReply(leaked);
check(got === '投降喵，你表情包比话还多 [[sticker:00a96eda]]', '真实泄漏原文只留下正话', got);

// 单行一律原样返回 —— 哪怕里面有英文，也不碰（避免误伤）
got = cleanReply('OK 啦，那就这么定了');
check(got === 'OK 啦，那就这么定了', '单行含英文不动', got);

got = cleanReply('压力大肥鱼是吧，我这鱼生也不容易啊  [[sticker:ad3beec6]]');
check(got === '压力大肥鱼是吧，我这鱼生也不容易啊  [[sticker:ad3beec6]]', '单行带表情标记不动', got);

// 多行但都是正话：取最后一行（提示词本来就要求单行）
got = cleanReply('第一句\n第二句');
check(got === '第二句', '多行时取最后一行', got);

// 全是思考过程：兜底取最后一行，不能返回空
got = cleanReply('Wait, let me think about this.\nActually the sender is unknown, keep it simple.');
check(got.length > 0, '全像思考时不返回空', got);

// 空输入
check(cleanReply('') === '', '空输入返回空');
check(cleanReply(null) === '', 'null 返回空');

// 带空行/空格的多行
got = cleanReply('  \n  \n真好笑\n  \n');
check(got === '真好笑', '忽略空行', got);

// 中文里掺一个英文词不算思考（只有 3+ 英文词才判）
got = cleanReply('我去 DeepSeek 官网看看\n说走就走');
check(got === '说走就走', '单个英文词不算，取最后一行', got);

console.log(`PASS ${checks} assertions on reply cleaning`);
