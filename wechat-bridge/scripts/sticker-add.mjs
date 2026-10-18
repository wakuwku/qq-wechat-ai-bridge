#!/usr/bin/env node
/**
 * sticker-add.mjs —— 往微信表情库里加表情
 *
 * 用法:
 *   node sticker-add.mjs --file "D:\\pic.png" --desc "无语扶额" --tags "无语,扶额"
 *   node sticker-add.mjs --import-dir "D:\\表情包" --source "本地导入"
 *   node sticker-add.mjs --list
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { StickerLibrary } from './sticker-lib.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, '..');
const DIR = path.join(REPO, 'stickers');

function arg(name, dflt = '') {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
}
const has = (name) => process.argv.includes(`--${name}`);

const lib = new StickerLibrary(DIR).load();

if (has('list')) {
  const items = lib.list({ limit: 200 });
  console.log(`表情库: ${DIR}`);
  console.log(`共 ${items.length} 个:`);
  for (const s of items) {
    console.log(`  ${s.id.slice(0, 8)}  ${String(s.desc || '(无描述)').padEnd(20)} [${(s.tags ?? []).join('/')}]  用过${s.usedCount ?? 0}次`);
  }
  process.exit(0);
}

const file = arg('file');
const importDir = arg('import-dir');

if (importDir) {
  if (!fs.existsSync(importDir)) { console.error('目录不存在:', importDir); process.exit(2); }
  const exts = ['.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp'];
  const files = fs.readdirSync(importDir).filter((f) => exts.includes(path.extname(f).toLowerCase()));
  let n = 0;
  for (const f of files) {
    const src = path.join(importDir, f);
    const dst = path.join(DIR, f);
    try {
      fs.mkdirSync(DIR, { recursive: true });
      if (path.resolve(src) !== path.resolve(dst)) fs.copyFileSync(src, dst);
      // 用文件名（去扩展名）当描述 —— 通常表情包文件名就是含义
      lib.upsert({ file: f, desc: path.basename(f, path.extname(f)), source: arg('source', '本地导入') });
      n++;
    } catch (e) {
      console.error(`  跳过 ${f}: ${e.message}`);
    }
  }
  console.log(`✅ 导入 ${n} 个表情（共 ${lib.list({ limit: 999 }).length} 个）`);
  process.exit(0);
}

if (file) {
  if (!fs.existsSync(file)) { console.error('文件不存在:', file); process.exit(2); }
  fs.mkdirSync(DIR, { recursive: true });
  const dst = path.join(DIR, path.basename(file));
  if (path.resolve(file) !== path.resolve(dst)) fs.copyFileSync(file, dst);
  const tags = arg('tags') ? arg('tags').split(/[,，\s]+/).filter(Boolean) : [];
  const id = lib.upsert({ file: path.basename(file), desc: arg('desc'), tags, source: arg('source', '手动添加'), note: arg('note') });
  console.log(`✅ 已添加，id=${id}`);
  process.exit(0);
}

console.log('用法: node sticker-add.mjs --file <图片> --desc <描述> --tags a,b');
console.log('      node sticker-add.mjs --import-dir <目录> [--source X]');
console.log('      node sticker-add.mjs --list');
