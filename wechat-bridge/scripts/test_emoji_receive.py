"""Offline tests: cache delay, unknown media, frame selection and scope isolation."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from PIL import Image, ImageDraw
from emoji_receive_media import EmojiMedia, emoji_md5, extract_frame
from receive_emoji import Receiver

def picture():
    image = Image.new('RGB', (32, 24), 'white')
    ImageDraw.Draw(image).rectangle((2, 2, 15, 15), fill='red')
    out = io.BytesIO()
    image.save(out, format='PNG')
    return out.getvalue()

class MediaTests(unittest.TestCase):
    def test_parse_rejects_non_hash_and_non_emoji(self):
        self.assertIsNone(emoji_md5('<img md5="'+'a'*32+'"/>'))
        self.assertIsNone(emoji_md5('<emoji md5="../../private"/>'))
        self.assertIsNone(emoji_md5('<emoji md5="&secret;"/>'))
        self.assertEqual(emoji_md5("sender:\n<msg><emoji md5='"+'A'*32+"'/></msg>"), 'a'*32)

    def test_first_frame_blank_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            first = Image.new('RGB', (32, 24), 'white')
            second = Image.open(io.BytesIO(picture())).copy()
            data = io.BytesIO()
            first.save(data, format='GIF', save_all=True, append_images=[second], duration=100)
            result = extract_frame(data.getvalue(), Path(temp)/'frame.png')
            self.assertEqual(result['frame_index'], 1)
            self.assertEqual(result['selection_reason'], 'first_frame_blank_fallback')
            with Image.open(result['image_path']) as result_image:
                self.assertEqual(result_image.format, 'PNG')

    def test_nonblank_first_frame_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            result = extract_frame(picture(), Path(temp)/'frame.png')
            self.assertEqual(result['frame_index'], 0)
            self.assertEqual(result['original_size'], [32,24])

    def test_unknown_cache_decode_and_hash_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            data=picture()
            md5=hashlib.md5(data).hexdigest()
            path=root/'account'/'cache'/'2026-10'/'Emoticon'/md5[:2]/md5
            path.parent.mkdir(parents=True)
            path.write_bytes(b'encrypted_fixture')
            resolver=EmojiMedia(root/'account',root/'out',decrypt=lambda _:data)
            result=resolver.resolve(md5,'filehelper')
            self.assertEqual(result['status'],'ready')
            self.assertFalse(result['known_in_library'])
            self.assertFalse(result['model_has_seen_image'])
            bad=EmojiMedia(root/'account',root/'bad',decrypt=lambda _:b'wrong')
            self.assertEqual(bad.resolve(md5,'filehelper')['status'],'decode_failed')
            self.assertFalse((root/'bad'/md5/'frame.png').exists())

    def test_missing_cache_does_not_get_lost_after_cursor_advances(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            class DB:
                account_dir=str(root/'account')
                rows=[{'local_id':1,'content':'baseline','sender_id':2}]
                def get_messages(self,chat,limit): return self.rows
            db=DB()
            data=picture()
            md5=hashlib.md5(data).hexdigest()
            resolver=EmojiMedia(db.account_dir,root/'out'/'images')
            receiver=Receiver(db,'filehelper',root/'out',resolver)
            receiver.poll()
            db.rows.append({'local_id':2,'content':f'<emoji md5="{md5}"/>','sender_id':2})
            receiver.poll()
            self.assertEqual(receiver.state['cursor'],2)
            self.assertEqual(len(receiver.state['pending']),1)
            # Restart retains pending work even though this message is below cursor.
            receiver=Receiver(db,'filehelper',root/'out',resolver)
            path=Path(db.account_dir)/'cache'/'2026-10'/'Emoticon'/md5[:2]/md5
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
            receiver.poll()
            receiver.poll()
            self.assertEqual(receiver.state['pending'],{})
            events=(root/'out'/'events.jsonl').read_text(encoding='utf-8').splitlines()
            self.assertEqual(len(events),2)
            self.assertTrue((root/'out'/'latest.json').exists())
            self.assertEqual(json.loads(events[-1])['media']['status'],'ready')
            with self.assertRaises(RuntimeError): Receiver(db,'another_chat',root/'out',resolver)

    def test_self_filter_and_explicit_filehelper_test(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            class DB:
                account_dir=str(root/'account')
                def get_messages(self,chat,limit):
                    return [{'local_id':1,'content':'<emoji md5="'+'a'*32+'"/>','sender_id':1}]
            db=DB()
            media=EmojiMedia(db.account_dir,root/'images')
            normal=Receiver(db,'chat',root/'normal',media)
            normal.poll(latest=True)
            self.assertFalse(normal.state['pending'])
            explicit=Receiver(db,'filehelper',root/'explicit',media,include_self=True)
            explicit.poll(latest=True)
            self.assertTrue(explicit.state['pending'])

if __name__=='__main__': unittest.main()
