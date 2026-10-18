import contextlib
import io
import json
import unittest
import urllib.error
from unittest.mock import Mock, patch
import send_native_emoji as client


class ClientTests(unittest.TestCase):
    def setUp(self):
        guard = patch.object(client, "authorize")
        guard.start()
        self.addCleanup(guard.stop)

    def invoke(self, args):
        with patch('sys.argv', ['client'] + args), contextlib.redirect_stdout(io.StringIO()) as output:
            result = client.main()
        return result, json.loads(output.getvalue())

    def test_send_preserves_id_and_normalizes_md5(self):
        with patch.object(client, 'call', return_value=(202, {'status': 'pending'})) as call:
            code, _ = self.invoke(['send', '--request-id', 'request-0001', '--md5', 'A'*32])
            self.assertEqual(code, 0)
            call.assert_called_once_with('/NativeEmoji/Send', {
                'request_id': 'request-0001', 'to_wxid': 'filehelper',
                'md5': 'a'*32, 'confirm': 'send_native_emoji'})

    def test_timeout_does_not_retry(self):
        opener = Mock()
        opener.open.side_effect = urllib.error.URLError('timeout')
        with patch.object(client.urllib.request, 'build_opener', return_value=opener):
            code, result = self.invoke(['send', '--request-id', 'request-0001', '--md5', 'a'*32])
        self.assertEqual(code, 2)
        self.assertEqual(result['request_id'], 'request-0001')
        opener.open.assert_called_once()

    def test_status_is_get_only(self):
        with patch.object(client, 'call', return_value=(200, {'replayed': True})) as call:
            self.invoke(['status', '--request-id', 'request-0001'])
            call.assert_called_once_with('/NativeEmoji/SendStatus?request_id=request-0001', None)

    def test_invalid_recipient_never_calls_http(self):
        with patch.object(client, 'call') as call, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.invoke(['send', '--request-id', 'request-0001', '--to', '../invalid', '--md5', 'a'*32])
            call.assert_not_called()


if __name__ == '__main__':
    unittest.main()
