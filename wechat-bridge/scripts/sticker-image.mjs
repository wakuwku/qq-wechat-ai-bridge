#!/usr/bin/env node
/**
 * sticker-image.mjs —— 把群里收到的表情变成「真的能喂给模型的图片」
 *
 * 三个环节：
 *   1. stickerMd5(text)       从消息 XML 里取表情 md5（<emoji md5="...">）
 *   2. decodeStickerImage()   调 decode_sticker.py，复用接收器那套解密/抽帧，产出首帧 PNG
 *   3. imagePart(path)        读 PNG → 规范 base64 → DSH prompt 的 image content part
 *
 * 协议依据（不是猜）：`@deepseek-ai/dsh-host-apiproxy` 的 promptContentPartSchema 接受
 *   { type:'image', mediaType:'image/png'|'image/jpeg'|'image/webp'|'image/gif',
 *     data:<规范 base64>, name? }
 * 宿主会校验 base64 的规范性，并把字节持久化成内容寻址附件。
 */
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PYTHON = process.env.DFY_PYTHON || 'python';
const CLI = path.join(__dirname, 'decode_sticker.py');
const IMAGE_CLI = path.join(__dirname, 'decode_image.py');
const OUT = path.join(__dirname, '..', 'state', 'sticker-images');
const OUT_IMAGES = path.join(__dirname, '..', 'state', 'chat-images');

// 单张图上限：太大就只给文字描述（表情首帧一般 100~300KB，这个阈值够用）
const MAX_IMAGE_BYTES = 1024 * 1024;

const MEDIA = { '.png': 'image/png', '.gif': 'image/gif', '.webp': 'image/webp', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg' };

/** 从消息内容里取表情 md5；不是表情消息返回 null */
export function stickerMd5(text) {
  const match = /<emoji\b[^>]*\bmd5="([0-9a-fA-F]{32})"/.exec(String(text ?? ''));
  return match ? match[1].toLowerCase() : null;
}

/** 是不是普通图片消息（<img>，不是表情） */
export function isImageMessage(text) {
  return /<img\b/i.test(String(text ?? ''));
}

/** 按 chat + localId 解普通图片。成功返回含 image_path 的结果，失败 null。 */
export function decodeChatImage(chatId, localId) {
  if (localId === undefined || localId === null || localId === '') return Promise.resolve(null);
  return new Promise((resolve) => {
    execFile(PYTHON, [IMAGE_CLI, '--chat', chatId, '--local-id', String(localId), '--out', OUT_IMAGES], {
      cwd: __dirname, windowsHide: true, timeout: 90_000,
      env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
    }, (err, stdout) => {
      let parsed = null;
      try { parsed = JSON.parse(String(stdout)); } catch { /* 非 JSON */ }
      if (!parsed || parsed.status !== 'ready' || !parsed.image_path || !fs.existsSync(parsed.image_path)) {
        return resolve(null);
      }
      resolve(parsed);
    });
  });
}

/** 按 md5 解码首帧 PNG。成功返回 resolve 结果（含 image_path），失败返回 null。 */
export function decodeStickerImage(md5, chatId) {
  return new Promise((resolve) => {
    execFile(PYTHON, [CLI, '--md5', md5, '--chat', chatId, '--out', OUT], {
      cwd: __dirname, windowsHide: true, timeout: 60_000,
      env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
    }, (err, stdout) => {
      let parsed = null;
      try { parsed = JSON.parse(String(stdout)); } catch { /* 非 JSON */ }
      if (!parsed || parsed.status !== 'ready' || !parsed.image_path || !fs.existsSync(parsed.image_path)) {
        return resolve(null);
      }
      resolve(parsed);
    });
  });
}

/** 读文件 → prompt 的 image content part。超限返回 null。 */
export function imagePart(imagePath, name) {
  const mediaType = MEDIA[path.extname(imagePath).toLowerCase()];
  if (!mediaType) return null;
  const bytes = fs.readFileSync(imagePath);
  if (!bytes.length || bytes.length > MAX_IMAGE_BYTES) return null;
  return {
    type: 'image',
    mediaType,
    data: bytes.toString('base64'),   // 规范 base64（无换行、带 padding）
    name: name || path.basename(imagePath),
  };
}

/**
 * 一步到位：消息文本 + localId → { kind, part, info }。
 *   kind = 'sticker' | 'image'；part 为 null 表示"是媒体消息但图没解出来"，
 *   调用方应退化成只给文字描述（不要凭空猜图里是什么）。
 *
 * 两种消息的解码路径不同：
 *   表情 <emoji md5="..."> —— md5 直接对应缓存文件
 *   图片 <img ...>        —— XML 里的 md5 是**原图** md5，本地文件名在消息行 packed_info 里，
 *                          所以必须走 chat + localId 反查（见 decode_image.py）
 */
export async function incomingMedia(text, chatId, localId) {
  const md5 = stickerMd5(text);
  if (md5) {
    const info = await decodeStickerImage(md5, chatId);
    if (!info) return { kind: 'sticker', md5, part: null, info: null };
    return { kind: 'sticker', md5, part: imagePart(info.image_path, `sticker-${md5.slice(0, 8)}.png`), info };
  }
  if (isImageMessage(text)) {
    const info = await decodeChatImage(chatId, localId);
    if (!info) return { kind: 'image', localId, part: null, info: null };
    return { kind: 'image', localId, part: imagePart(info.image_path, `image-${localId}.png`), info };
  }
  return null;
}
