#!/usr/bin/env node
/**
 * dsh-endpoint.mjs —— 自动找到 DSH Web 的地址，不再写死端口
 *
 * 为什么需要：DSH 桌面版的 web 端口是**动态的**。
 * 2026-10-17 还是 19387，10-18 用户更新并重启后变成 53508 —— 写死必然失效，
 * 而且失效方式是「桥接启动后一直 401/ECONNREFUSED」，很容易误判成"桥接坏了"。
 *
 * 识别 DSH 的特征（实测）：GET / 返回 401，正文是
 *   "dsh web authentication required; reopen the URL printed by dsh web."
 * 这个字符串足够独特，可以据此在监听端口里认出 DSH。
 *
 * 输出：解析出的 baseUrl；同时写 state/dsh-baseurl.json 缓存，并把 qq-bridge/config.json
 * 的 dsh.baseUrl 改成当前值（桥接和微信闭环都从这里读，单一真相源）。
 */
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const STATE = path.resolve(__dirname, '..', 'state');
fs.mkdirSync(STATE, { recursive: true });
const CACHE = path.join(STATE, 'dsh-baseurl.json');
const QQ_CONFIG = path.resolve(__dirname, '..', 'config.json');

const SIGNATURE = 'dsh web authentication required';
const KNOWN_PORTS = [19387];

function runToFile(exe, args, timeoutMs = 10_000) {
  const tmp = path.join(STATE, 'tmp-dsh-endpoint.out');
  try { fs.rmSync(tmp, { force: true }); } catch { /* ignore */ }
  let fd;
  try { fd = fs.openSync(tmp, 'w'); } catch { return ''; }
  try {
    spawnSync(exe, args, { windowsHide: true, timeout: timeoutMs, stdio: ['ignore', fd, fd] });
  } catch { /* ignore */ } finally {
    try { fs.closeSync(fd); } catch { /* ignore */ }
  }
  try { return fs.readFileSync(tmp, 'utf8'); } catch { return ''; }
}

/** 本机所有 LISTENING 的端口（去重） */
function listeningPorts() {
  const out = runToFile('netstat', ['-ano']);
  const ports = new Set();
  for (const line of out.split(/\r?\n/)) {
    const match = /^\s*TCP\s+(\S+):(\d+)\s+\S+\s+LISTENING/i.exec(line);
    if (!match) continue;
    const address = match[1];
    if (address !== '127.0.0.1' && address !== '0.0.0.0' && address !== '[::1]') continue;
    ports.add(Number(match[2]));
  }
  return [...ports];
}

/** 这个端口是不是 DSH */
async function looksLikeDsh(port, timeoutMs = 1200) {
  try {
    const response = await fetch(`http://127.0.0.1:${port}/`, {
      signal: AbortSignal.timeout(timeoutMs),
      redirect: 'manual',
    });
    const text = (await response.text()).slice(0, 300);
    return text.toLowerCase().includes(SIGNATURE);
  } catch {
    return false;
  }
}

function readJson(file, fallback = null) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return fallback; }
}

/**
 * @param {{preferred?:string, forceDiscover?:boolean}} options
 * @returns {Promise<{url:string, source:'env'|'preferred'|'cache'|'discovered'|'none'}>}
 */
export async function resolveDshBaseUrl(options = {}) {
  const envUrl = process.env.DFY_DSH_BASEURL;
  if (envUrl && await looksLikeDsh(new URL(envUrl).port)) {
    return { url: envUrl.replace(/\/$/, ''), source: 'env' };
  }

  const preferred = options.preferred || '';
  if (preferred && !options.forceDiscover) {
    const port = Number(new URL(preferred).port);
    if (port && await looksLikeDsh(port)) return { url: preferred.replace(/\/$/, ''), source: 'preferred' };
  }

  const cached = readJson(CACHE, null)?.url;
  if (cached && !options.forceDiscover) {
    const port = Number(new URL(cached).port);
    if (port && await looksLikeDsh(port)) {
      return { url: cached.replace(/\/$/, ''), source: 'cache' };
    }
  }

  const ports = listeningPorts();
  // 先试已知默认端口，再按端口号从小到大扫（避免每次都扫几百个）
  const ordered = [...new Set([...KNOWN_PORTS.filter((p) => ports.includes(p)), ...ports.sort((a, b) => a - b)])];
  for (const port of ordered) {
    if (await looksLikeDsh(port)) {
      return { url: `http://127.0.0.1:${port}`, source: 'discovered' };
    }
  }
  return { url: '', source: 'none' };
}

/**
 * 把解析结果落到缓存（微信自己读这个），并按需同步 qq-bridge 的配置。
 *
 * `writeQqConfig` 默认 true 只为兼容「桌面一键启动」——那是用户同时拉起 QQ+微信的组合入口。
 * 微信看护自己调用时**必须传 false**：QQ 和微信是两条独立维护的线，
 * 微信侧不应该去改 qq-bridge/config.json，否则两边会互相覆盖。
 */
export function applyDshBaseUrl(url, { writeQqConfig = true } = {}) {
  const result = { url, appliedAt: new Date().toISOString() };
  try {
    fs.mkdirSync(STATE, { recursive: true });
    fs.writeFileSync(CACHE, JSON.stringify(result, null, 2));
  } catch { /* ignore */ }

  if (!writeQqConfig) return false;

  let changed = false;
  const config = readJson(QQ_CONFIG, null);
  if (config && config.dsh && config.dsh.baseUrl !== url) {
    config.dsh.baseUrl = url;
    try {
      fs.copyFileSync(QQ_CONFIG, QQ_CONFIG + '.bak-before-dsh-port');
      fs.writeFileSync(QQ_CONFIG, JSON.stringify(config, null, 2));
      changed = true;
    } catch { /* ignore */ }
  }
  return changed;
}

/** 一步到位：解析 + 落盘，返回最终使用的 url */
export async function syncDshBaseUrl(options = {}) {
  const { url, source } = await resolveDshBaseUrl(options);
  if (!url) return { url: '', source, changed: false };
  const changed = applyDshBaseUrl(url, { writeQqConfig: options.writeQqConfig !== false });
  return { url, source, changed };
}

// 直接运行时打印解析结果，方便排查
if (process.argv[1] && process.argv[1].endsWith('dsh-endpoint.mjs')) {
  syncDshBaseUrl({ forceDiscover: process.argv.includes('--force') })
    .then((result) => {
      console.log(JSON.stringify(result, null, 2));
      process.exit(result.url ? 0 : 1);
    })
    .catch((error) => { console.error(error?.message ?? error); process.exit(1); });
}
