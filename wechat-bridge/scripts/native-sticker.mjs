#!/usr/bin/env node
/**
 * native-sticker.mjs —— 原生 type47 表情发送（从 loop.mjs 抽出来，便于单独验证）
 *
 * 为什么单独一个模块：
 *   loop.mjs 一被 import 就会跑 main()，没法对它里面的发送逻辑做单点测试。
 *   抽出来后，`native_sticker_test.mjs` 可以用**完全相同的代码**验证：
 *     - 有原生 md5 的表情 → sent
 *     - md5 不在收藏里   → not_sent（可以安全回落发图片）
 *     - 结果不明         → unknown（绝不回落、绝不重发）
 *
 * 返回三态而不是布尔值，是因为「发送失败」和「不知道发没发」必须区别对待：
 * 前者可以回落，后者回落就会重复发。
 */
import { createHash, randomUUID } from 'node:crypto';
import { execFile } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PYTHON = process.env.DFY_PYTHON || 'python';
const CLIENT = path.join(__dirname, 'send_native_emoji.py');

/** 一次性 request_id（8–64 位小写字母/数字/_/-）。一次发送意图只用一个。 */
export function newRequestId(intent) {
  return intent ? 'wx-' + createHash('sha256').update(String(intent)).digest('hex').slice(0, 48)
    : 'wx-' + randomUUID();
}

export function runNativeClient(args, timeoutMs = 60_000) {
  return new Promise((resolve) => {
    execFile(PYTHON, [CLIENT, ...args], {
      cwd: __dirname, windowsHide: true, timeout: timeoutMs,
      env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
    }, (err, stdout, stderr) => {
      let parsed = null;
      try { parsed = JSON.parse(String(stdout)); } catch { /* 非 JSON 输出 */ }
      resolve({ err, parsed, stdout: String(stdout || ''), stderr: String(stderr || '') });
    });
  });
}

/**
 * @returns {'sent'|'not_sent'|'unknown'}
 */
async function attemptOnce(chatId, md5, log, intent) {
  const requestId = newRequestId(intent);
  const first = await runNativeClient(['send', '--request-id', requestId, '--to', chatId, '--md5', md5]);
  let result = first.parsed?.result ?? null;
  // 传输失败或结果未定：用【同一个】ID 查一次，绝不新建 ID 重试
  if (!result || (result.terminal !== true && result.submitted !== true)) {
    const again = await runNativeClient(['status', '--request-id', requestId]);
    result = again.parsed?.result ?? result;
  }
  if (!result) {
    log(`表情(原生) 结果未知：客户端输出无法解析，request_id=${requestId}（保留待查，不重试）`);
    return { state: 'unknown', requestId };
  }
  if (result.submitted === true) return { state: 'sent', requestId };
  if (result.terminal === true && result.submission_attempted === false && result.submitted === false) {
    log(`表情(原生) 未提交：${result.status} request_id=${requestId}`);
    return { state: 'not_sent', requestId, status: result.status };
  }
  log(`表情(原生) 结果未知：status=${result.status} request_id=${requestId}（不回落、不重试）`);
  return { state: 'unknown', requestId, status: result.status };
}

/** Never create a new ID to retry an accepted business intent. */
export async function sendStickerNative(chatId, md5, log = () => {}, intent) {
  return (await attemptOnce(chatId, md5, log, intent)).state;
}
