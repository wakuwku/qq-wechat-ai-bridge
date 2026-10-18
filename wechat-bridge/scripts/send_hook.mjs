#!/usr/bin/env node
/**
 * send_hook.mjs —— 用 hook 内部 API 发送微信消息（替代慢的 UI 自动化）
 *
 * 对比：
 *   wechatauto/UI 自动化：10+ 秒/条、抢焦点、必须开窗口
 *   hook 内部 API：毫秒级、不抢焦点、窗口可最小化
 *
 * 用法:
 *   node send_hook.mjs --to 12345678901@chatroom --text "你好"
 *   node send_hook.mjs --status
 */
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PY = path.join(__dirname, 'wxhook_send.py');

function runPython(args) {
  return new Promise((resolve) => {
    // 用绝对路径：PATH 里的 python 没装 wechatauto/pycryptodome
    const p = spawn(process.env.DFY_PYTHON || 'python', [PY, ...args], {
      cwd: __dirname,
      windowsHide: true,
      env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
    });
    let out = '';
    let err = '';
    p.stdout.on('data', (d) => { out += d.toString('utf8'); });
    p.stderr.on('data', (d) => { err += d.toString('utf8'); });
    p.on('error', (error) => resolve({ code: -1, out, err: error.message }));
    const timer = setTimeout(() => p.kill(), 35000);
    p.on('close', (code) => { clearTimeout(timer); resolve({ code, out, err }); });
  });
}

export async function sendViaHook(chatId, text) {
  const r = await runPython(['--to', chatId, '--text', text]);
  const ok = r.code === 0 && (/✅ 已发送/.test(r.out) || /"ret":\s*0/.test(r.out));
  return { ok, code: r.code, out: r.out, err: r.err };
}

// 直接运行时作为 CLI
if (process.argv[1] && process.argv[1].endsWith('send_hook.mjs')) {
  const a = process.argv.slice(2);
  if (a.includes('--status')) {
    const r = await runPython(['--status']);
    console.log(r.out || r.err);
    process.exit(0);
  }
  const to = a[a.indexOf('--to') + 1];
  const text = a[a.indexOf('--text') + 1];
  if (!to || !text) {
    console.error('用法: node send_hook.mjs --to <chatId> --text <内容>');
    process.exit(2);
  }
  const r = await sendViaHook(to, text);
  console.log(r.out || r.err);
  process.exit(r.ok ? 0 : 1);
}
