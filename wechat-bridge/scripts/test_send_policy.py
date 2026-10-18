import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from send_policy import authorize


class PolicyTests(unittest.TestCase):
    def test_missing_config_denies(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {'DFY_CONFIG_PATH': str(Path(directory)/'config.json')}):
            with self.assertRaises(ValueError): authorize('filehelper')

    def test_reloads_revocation_and_expiry(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)/'config.json'
            with patch.dict(os.environ, {'DFY_CONFIG_PATH': str(config)}):
                for expiry, allowed in [(None, True), (1, False), ('invalid', False), ('NaN', False)]:
                    config.write_text(json.dumps({'whitelist': {'chats': [{'chatId': 'filehelper', 'expiresAt': expiry}]}}))
                    if allowed: authorize('filehelper')
                    else:
                        with self.assertRaises(ValueError): authorize('filehelper')
                config.write_text('{"whitelist":{"chats":[]}}')
                with self.assertRaises(ValueError): authorize('filehelper')

    def test_other_recipient_denied(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)/'config.json'
            config.write_text('{"whitelist":{"chats":[{"chatId":"filehelper"}]}}')
            with patch.dict(os.environ, {'DFY_CONFIG_PATH': str(config)}):
                with self.assertRaises(ValueError): authorize('12345678901@chatroom')


if __name__ == '__main__': unittest.main()
