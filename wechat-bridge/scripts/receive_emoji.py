"""Read-only selected-chat receiver; outputs PNG attachments, never replies."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import time

from emoji_receive_media import EmojiMedia, atomic_json, emoji_md5

REPO = Path(__file__).resolve().parents[1]

class Receiver:
    def __init__(self, db, chat, output, media, include_self=False):
        self.db, self.chat, self.output, self.media = db, chat, Path(output), media
        self.include_self = include_self
        self.output.mkdir(parents=True, exist_ok=True)
        self.state_path = self.output / 'cursor.json'
        self.scope = {'account': str(Path(db.account_dir).resolve()), 'chat': chat}
        self.state = json.loads(self.state_path.read_text(encoding='utf-8')) if self.state_path.exists() else {
            'scope': self.scope, 'cursor': None, 'pending': {}, 'events': {}}
        if self.state['scope'] != self.scope:
            raise RuntimeError('Output belongs to a different account/chat')

    def emit(self, key, record, result):
        signature = result['status']
        if self.state['events'].get(key) == signature:
            return
        event = {'event_id': key + ':' + signature, 'at': datetime.datetime.now().astimezone().isoformat(),
                 'chat_id': self.chat, 'local_id': record['local_id'], 'media': result}
        # Deterministic per-event JSON is authoritative; JSONL consumers dedupe event_id.
        atomic_json(self.output / 'events' / (key + '-' + signature + '.json'), event)
        with (self.output / 'events.jsonl').open('a', encoding='utf-8') as f:
            f.write(json.dumps(event, ensure_ascii=False) + '\n')
            f.flush()
            os.fsync(f.fileno())
        self.state['events'][key] = signature
        if signature == 'ready':
            atomic_json(self.output / 'latest.json', event)
            image = Path(result['image_path']).as_posix()
            (self.output / '最新表情预览.md').write_text(
                '# 收到的表情首帧\n\n'
                f'消息编号：{record["local_id"]}；MD5：`{result["md5"]}`；'
                f'取第{result["frame_index"] + 1}帧（总帧数：{result["frame_count"] if result["frame_count"] is not None else "未统计"}）。\n\n'
                f'![收到的表情](<{image}>)\n\n图片已导出；是否已被模型查看需由消费端确认。\n', encoding='utf-8')
        print(json.dumps(event, ensure_ascii=True), flush=True)

    def poll(self, latest=False):
        rows = sorted(self.db.get_messages(self.chat, limit=100), key=lambda x: x.get('local_id', 0))
        if latest:
            rows = [r for r in rows if emoji_md5(r.get('content', ''))][-1:]
        elif self.state['cursor'] is None:
            self.state['cursor'] = max([r.get('local_id', 0) for r in rows] + [0])
            atomic_json(self.state_path, self.state)
            print(json.dumps({'status': 'watching_new_messages', 'chat_id': self.chat,
                              'cursor': self.state['cursor'], 'sends_messages': False}), flush=True)
            return
        for row in rows:
            lid = row.get('local_id', 0)
            if not latest and lid <= self.state['cursor']:
                continue
            self.state['cursor'] = max(self.state['cursor'] or 0, lid)
            if not self.include_self and row.get('sender_id') == 1:
                continue
            md5 = emoji_md5(row.get('content', ''))
            if md5:
                key = hashlib.sha256((self.chat + ':' + str(lid) + ':' + md5).encode()).hexdigest()[:24]
                if self.state['events'].get(key) != 'ready':
                    self.state['pending'].setdefault(key, {'local_id': lid, 'md5': md5, 'attempts': 0})
        for key, record in list(self.state['pending'].items()):
            result = self.media.resolve(record['md5'], self.chat)
            record['attempts'] += 1
            if result['status'] != 'ready' and record['attempts'] >= 60:
                result['last_failure'] = result['status']
                result['status'] = 'unavailable_after_retries'
            self.emit(key, record, result)
            if result['status'] in ('ready', 'unavailable_after_retries'):
                del self.state['pending'][key]
        atomic_json(self.state_path, self.state)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--chat', required=True)
    p.add_argument('--account-dir', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--include-self', action='store_true', help='Required for phone-to-filehelper tests')
    p.add_argument('--once', action='store_true')
    p.add_argument('--latest', action='store_true', help='Explicitly inspect latest sticker instead of baselining')
    p.add_argument('--interval', type=float, default=3)
    p.add_argument('--ignore-library', action='store_true', help='Test decoding directly from WeChat cache')
    args = p.parse_args()
    from wechatauto import WeChatDB
    db = WeChatDB(db_dir=str(args.account_dir.parent), account=args.account_dir.name) if args.account_dir else WeChatDB()
    # Keep one writer per output directory. The OS releases the lock on exit.
    args.out.mkdir(parents=True, exist_ok=True)
    import msvcrt
    lock = (args.out / 'receiver.lock').open('a+b')
    lock.seek(0, 2)
    if lock.tell() == 0:
        lock.write(b'0')
        lock.flush()
    lock.seek(0)
    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    media = EmojiMedia(db.account_dir, args.out / 'images', None if args.ignore_library else
                      REPO / 'stickers' / '检索库素材' / 'sticker-catalog.json', db=db)
    receiver = Receiver(db, args.chat, args.out, media, args.include_self)
    while True:
        try:
            receiver.poll(latest=args.latest)
        except Exception as exc:
            if args.once or args.latest:
                raise
            print(json.dumps({'status':'poll_error','error_type':type(exc).__name__,
                              'sends_messages':False}),flush=True)
        if args.once or args.latest:
            return
        if (args.out / 'STOP').exists():
            return
        time.sleep(max(args.interval, 1))

if __name__ == '__main__':
    main()
