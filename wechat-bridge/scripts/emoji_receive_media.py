"""Local-only sticker decoding and first-frame extraction. Never sends messages."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

from PIL import Image

MAX_BYTES = 32 * 1024 * 1024
MAX_PIXELS = 16_000_000
HASH = re.compile(r'^[0-9a-f]{32}$')

def emoji_md5(text):
    if not isinstance(text, str) or len(text) > 128 * 1024:
        return None
    match = re.search(r'<emoji\b[^>]{0,32768}>', text, re.I)
    if not match:
        return None
    tag = match.group(0)
    if not tag.endswith('/>'):
        tag = tag[:-1] + '/>'
    try:
        value = ET.fromstring(tag).attrib.get('md5', '').lower()
        return value if HASH.fullmatch(value) else None
    except ET.ParseError:
        return None

def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + '.tmp')
    with temp.open('w', encoding='utf-8') as f:
        json.dump(value, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, path)

def extract_frame(data, output):
    if len(data) > MAX_BYTES:
        raise ValueError('media_too_large')
    with Image.open(io.BytesIO(data)) as image:
        if image.width * image.height > MAX_PIXELS:
            raise ValueError('image_dimensions_too_large')
        count = getattr(image, 'n_frames', 1)
        first = image.convert('RGBA')
        selected, index, reason = first, 0, 'first_frame'
        def blank(frame):
            if frame.getchannel('A').getbbox() is None:
                return True
            extrema = frame.convert('RGB').getextrema()
            return all(low == high for low, high in extrema)
        if blank(first) and count > 1:
            reason = 'sampled_frames_uniform_or_transparent'
            for idx in range(1, min(count, 32)):
                image.seek(idx)
                candidate = image.convert('RGBA')
                if not blank(candidate):
                    selected, index, reason = candidate, idx, 'first_frame_blank_fallback'
                    break
        original_size = list(selected.size)
        selected.thumbnail((1280, 1280))
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        temp = output.with_suffix('.tmp')
        selected.save(temp, format='PNG')
        os.replace(temp, output)
        return {'image_path': str(output.resolve()), 'mime_type': 'image/png',
                'frame_index': index, 'frame_count': count, 'selection_reason': reason,
                'original_size': original_size, 'preview_size': list(selected.size)}

def extract_wxgf(data, output):
    """WeChat may store a GIF as WXGF/HEVC with a different payload hash."""
    import imageio_ffmpeg
    if len(data) > MAX_BYTES:
        raise ValueError('media_too_large')
    with tempfile.TemporaryDirectory(prefix='wechat-frame-') as temp:
        root = Path(temp)
        source = root / 'input.h265'
        source.write_bytes(data)
        command = [imageio_ffmpeg.get_ffmpeg_exe(), '-hide_banner', '-loglevel', 'error',
                   '-nostdin', '-threads', '2', '-f', 'hevc', '-i', str(source),
                   '-vf', r'scale=w=min(1280\,iw):h=min(1280\,ih):force_original_aspect_ratio=decrease',
                   '-frames:v', '32', '-y', str(root/'frame-%03d.png')]
        # Retain the whole WXGF first, as in the existing exporter; cutting may
        # discard parameter sets. This decoder only opens our local temp file.
        run = subprocess.run(command, capture_output=True, timeout=30,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        frames = sorted(root.glob('frame-*.png'))
        if run.returncode or not frames:
            raise ValueError('wxgf_decode_failed')
        chosen, reason = 0, 'first_frame'
        for idx, path in enumerate(frames):
            with Image.open(path) as image:
                extrema = image.convert('RGB').getextrema()
                nonblank = any(low != high for low, high in extrema)
            if nonblank:
                chosen = idx
                reason = 'first_frame_blank_fallback' if idx else 'first_frame'
                break
        else:
            reason = 'sampled_frames_uniform_or_transparent'
        result = extract_frame(frames[chosen].read_bytes(), output)
        result.update(frame_index=chosen, frame_count=None, sampled_frame_count=len(frames),
                      selection_reason=reason, container_format='wxgf', original_size=None)
        return result

class EmojiMedia:
    def __init__(self, account_dir, output_dir, catalog=None, db=None, decrypt=None):
        self.account = Path(account_dir).resolve()
        self.output = Path(output_dir).resolve()
        self.db, self.decrypt_override = db, decrypt
        self.key = None
        self.last_key_probe = 0
        self.known = {}
        if catalog and Path(catalog).is_file():
            data = json.loads(Path(catalog).read_text(encoding='utf-8'))
            self.known = {x['md5']: x for x in data['items']}

    def candidates(self, md5, chat_id):
        if not HASH.fullmatch(md5):
            return []
        prefix = md5[:2]
        patterns = [f'cache/*/Emoticon/{prefix}/{md5}',
                    f'cache/*/Emoticon/{prefix}/{md5}.dat']
        for area in ['Persist', 'Temp', 'Thumb']:
            patterns.append(f'business/emoticon/{area}/{prefix}/{md5}')
        chat_hash = hashlib.md5(chat_id.encode()).hexdigest()
        patterns.extend([f'msg/attach/{chat_hash}/*/Img/{md5}.dat',
                         f'msg/attach/{chat_hash}/*/Img/{md5}_t.dat'])
        result = []
        for pattern in patterns:
            for path in self.account.glob(pattern):
                try:
                    path.resolve().relative_to(self.account)
                    if path.is_file() and 0 < path.stat().st_size <= MAX_BYTES and path not in result:
                        result.append(path)
                except (OSError, ValueError):
                    continue
        return result[:24]

    def decode(self, path):
        data = path.read_bytes()
        if data.startswith((b'GIF8', b'\x89PNG', b'\xff\xd8\xff', b'RIFF', b'BM')):
            return data
        if self.decrypt_override:
            return self.decrypt_override(path)
        # Reuse the existing emoticon AES implementation; no keys in events/logs.
        if len(data) % 16 == 0:
            from Crypto.Cipher import AES
            from Crypto.Util.Padding import unpad
            if self.key is None and self.db is not None:
                script = Path(__file__).resolve().parents[1] / 'vendor' / 'wechat-emoticon-parser' / 'wechat_emoticon_export.py'
                spec = importlib.util.spec_from_file_location('receive_emoticon_decoder', script)
                parser = importlib.util.module_from_spec(spec)
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    spec.loader.exec_module(parser)
                    cfg = getattr(self.db, 'cfg_dword', None)
                    if cfg:
                        candidate = parser.derive_key(cfg, self.db.wxid)
                        if parser.verify_key(candidate, data[:16]):
                            self.key = candidate
                    if self.key is None and time.monotonic() - self.last_key_probe > 60:
                        self.last_key_probe = time.monotonic()
                        found = parser.find_key_from_memory(data[:16], self.db.wxid)
                        if found:
                            self.key = found[1]
            if self.key:
                try:
                    return unpad(AES.new(self.key, AES.MODE_CBC, self.key).decrypt(data), 16)
                except ValueError:
                    pass
        if self.db is not None:
            from wechatauto.media import MediaDownloader
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return MediaDownloader(self.db, save_dir=str(self.output)).decrypt_image(str(path))
        raise ValueError('no_decoder')

    def resolve(self, md5, chat_id):
        if not HASH.fullmatch(md5):
            return {'status': 'invalid_md5'}
        common = {'md5': md5, 'known_in_library': md5 in self.known,
                  'sends_messages': False, 'model_has_seen_image': False}
        item = self.known.get(md5)
        if item:
            common['library_number'] = item['number']
            common['library_meaning'] = item.get('suggested_use', '')
        paths = self.candidates(md5, chat_id)
        if item and Path(item['original_path']).is_file():
            paths.insert(0, Path(item['original_path']))
        if not paths:
            return dict(common, status='cache_missing')
        errors = []
        for path in paths:
            try:
                if path.stat().st_size > MAX_BYTES:
                    continue
                data = self.decode(path)
                payload_md5 = hashlib.md5(data).hexdigest()
                converted_container = data.startswith(b'wxgf') and path in self.candidates(md5, chat_id)
                if payload_md5 != md5 and not converted_container:
                    errors.append('content_hash_mismatch')
                    continue
                extractor = extract_wxgf if data.startswith(b'wxgf') else extract_frame
                result = extractor(data, self.output / md5 / 'frame.png')
                result.update(common, status='ready', source_hash_verified=payload_md5 == md5,
                              decoded_content_md5=payload_md5,
                              identity_basis='exact_content_md5' if payload_md5 == md5 else 'wechat_cache_filename_wxgf')
                atomic_json(self.output / md5 / 'result.json', result)
                return result
            except Exception as exc:
                # Exception text from decoders may contain paths/keys; use type only.
                errors.append(type(exc).__name__)
        return dict(common, status='decode_failed', error_types=sorted(set(errors)))
