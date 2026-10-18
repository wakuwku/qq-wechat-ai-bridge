"""Recheck the local conversation whitelist immediately before each send."""
import json
import math
import os
from pathlib import Path
import time


def authorize(recipient):
    path = Path(os.environ.get('DFY_CONFIG_PATH', str(Path(__file__).resolve().parents[1] / 'config.json')))
    try:
        cfg = json.loads(path.read_text(encoding='utf-8-sig'))
        for entry in cfg.get('whitelist', {}).get('chats', []):
            if entry.get('chatId') != recipient:
                continue
            expiry = entry.get('expiresAt')
            if expiry not in (None, ''):
                expiry = float(expiry)
                if not math.isfinite(expiry) or expiry <= time.time() * 1000:
                    raise ValueError('expired_or_invalid_whitelist_entry')
            return
    except (OSError, ValueError, TypeError, AttributeError) as error:
        raise ValueError('recipient_permission_unavailable') from error
    raise ValueError('recipient_not_allowed')
