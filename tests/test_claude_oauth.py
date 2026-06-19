import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


DAEMON_DIR = Path(__file__).resolve().parents[1] / "daemon"
if str(DAEMON_DIR) not in sys.path:
    sys.path.insert(0, str(DAEMON_DIR))

import claude_oauth


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def read(self):
        return json.dumps(self.payload).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


class ClaudeOAuthTests(unittest.TestCase):
    def test_get_access_token_refreshes_proactively_when_expiring_soon(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            credentials_file = Path(tmpdir) / "credentials.json"
            credentials_file.write_text(json.dumps({
                "claudeAiOauth": {
                    "accessToken": "stale-token",
                    "refreshToken": "refresh-token",
                    "expiresAt": 1_700_000_240_000,
                    "subscriptionType": "pro",
                    "scopes": ["user:inference"],
                },
                "other": {"keep": True},
            }))

            with mock.patch.object(claude_oauth.time, "time", return_value=1_700_000_000):
                with mock.patch.object(
                    claude_oauth.urllib.request,
                    "urlopen",
                    return_value=FakeResponse({
                        "access_token": "fresh-token",
                        "refresh_token": "new-refresh-token",
                        "expires_in": 7200,
                    }),
                ):
                    token, oauth = claude_oauth.get_access_token(credentials_file)

            persisted = json.loads(credentials_file.read_text())

        self.assertEqual(token, "fresh-token")
        self.assertEqual(oauth["refreshToken"], "new-refresh-token")
        self.assertEqual(persisted["claudeAiOauth"]["accessToken"], "fresh-token")
        self.assertEqual(persisted["claudeAiOauth"]["refreshToken"], "new-refresh-token")
        self.assertEqual(persisted["claudeAiOauth"]["subscriptionType"], "pro")
        self.assertEqual(persisted["other"], {"keep": True})

    def test_get_access_token_skips_refresh_when_token_is_not_near_expiry(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            credentials_file = Path(tmpdir) / "credentials.json"
            credentials_file.write_text(json.dumps({
                "claudeAiOauth": {
                    "accessToken": "valid-token",
                    "refreshToken": "refresh-token",
                    "expiresAt": 1_700_100_000_000,
                }
            }))

            with mock.patch.object(claude_oauth.time, "time", return_value=1_700_000_000):
                with mock.patch.object(claude_oauth.urllib.request, "urlopen") as mocked_urlopen:
                    token, oauth = claude_oauth.get_access_token(credentials_file)

        self.assertEqual(token, "valid-token")
        self.assertEqual(oauth["accessToken"], "valid-token")
        mocked_urlopen.assert_not_called()
