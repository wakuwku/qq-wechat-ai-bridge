// Portable foreground supervisor; never launches clients or changes autostart.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, execFileSync } from 'node:child_process';
import { syncDshBaseUrl } from './dsh-endpoint.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));
const config = JSON.parse(fs.readFileSync(path.join(root, 'config.json'), 'utf8').replace(/^\uFEFF/, ''));
const chats = (config.whitelist?.chats ?? []).filter(x => x.chatId &&
  (x.expiresAt == null || x.expiresAt === '' || (Number.isFinite(Number(x.expiresAt)) && Number(x.expiresAt) > Date.now()))).map(x => x.chatId);
if (!chats.length) throw new Error('No active whitelist entries; refusing to start.');
if (process.platform !== 'win32') throw new Error('WeChat bridge requires Windows.');
const addresses = execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
  'Get-NetTCPConnection -LocalPort 30001 -State Listen -ErrorAction Stop | Select-Object -ExpandProperty LocalAddress'],
  { encoding: 'utf8', windowsHide: true, timeout: 10000 }).trim().split(/\s+/);
if (!addresses.length || addresses.some(x => !['127.0.0.1', '::1'].includes(x))) {
  throw new Error('Hook must listen ONLY on loopback. See docs/HOOK-DEPENDENCY.md; do not expose it to LAN.');
}
const state = path.join(root, 'state'); fs.mkdirSync(state, {recursive:true});
const lock = path.join(state, 'release-launcher.lock');
const fd = fs.openSync(lock, 'wx'); fs.writeFileSync(fd, String(process.pid)); fs.closeSync(fd);
const children = [];
let ending = false;
function stop(code=0) {
  if (ending) return; ending = true;
  for (const child of children) child.kill();
  fs.unlinkSync(lock); process.exitCode=code;
}
process.on('SIGINT', () => stop()); process.on('SIGTERM', () => stop());
try {
  const endpoint = await syncDshBaseUrl({preferred: config.dsh?.baseUrl, writeQqConfig:false});
  if (!endpoint.url) throw new Error('DSH endpoint unavailable');
  const options = {cwd:root, windowsHide:true, stdio:'inherit', env:{...process.env, PYTHONIOENCODING:'utf-8'}};
  children.push(spawn(process.env.DFY_PYTHON || 'python', [path.join(root,'scripts/watch_wechat.py'),
    '--chat', ...chats, '--out',path.join(state,'queue.jsonl'),'--cursor',path.join(state,'cursor.json'),
    '--names',path.join(state,'names.json')], options));
  children.push(spawn(process.execPath,[path.join(root,'scripts/loop.mjs')],options));
  for (const child of children) {
    child.on('error',error=>{console.error(error.message);stop(1);});
    child.on('exit',code=>stop(code || 0));
  }
} catch(error) { console.error(error.message); stop(1); }
