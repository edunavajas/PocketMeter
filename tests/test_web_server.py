import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "daemon" / "web-server.py"
SPEC = importlib.util.spec_from_file_location("pocketmeter_web_server", MODULE_PATH)
WEB_SERVER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(WEB_SERVER)


class WebServerResolveHostTests(unittest.TestCase):
    def test_resolve_esp_host_prefers_override_env(self):
        with mock.patch.dict(
            WEB_SERVER.os.environ,
            {"POCKETMETER_ESP_HOST": "override.local", "ESP32_HOST": "other.local"},
            clear=False,
        ):
            host, source = WEB_SERVER.resolve_esp_host()

        self.assertEqual(host, "override.local")
        self.assertEqual(source, "env:POCKETMETER_ESP_HOST")

    def test_resolve_esp_host_uses_cached_host_when_no_env_override(self):
        with mock.patch.dict(WEB_SERVER.os.environ, {}, clear=True):
            with mock.patch.object(WEB_SERVER, "load_configured_host", return_value=("192.168.1.44", "cache")):
                host, source = WEB_SERVER.resolve_esp_host()

        self.assertEqual(host, "192.168.1.44")
        self.assertEqual(source, "cache")

    def test_resolve_esp_host_falls_back_to_default(self):
        with mock.patch.dict(WEB_SERVER.os.environ, {}, clear=True):
            with mock.patch.object(WEB_SERVER, "load_configured_host", return_value=(None, None)):
                host, source = WEB_SERVER.resolve_esp_host()

        self.assertEqual(host, WEB_SERVER.DEFAULT_ESP_HOST)
        self.assertEqual(source, "default")

    def test_save_key_normalizes_wrapped_bearer_token(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            keys_file = Path(tmpdir) / "api-keys.json"
            with mock.patch.object(WEB_SERVER, "KEYS_FILE", keys_file):
                WEB_SERVER.save_key("kimi", "  'Bearer sk-kimi-test'  ")

            saved = json.loads(keys_file.read_text())

        self.assertEqual(saved["kimi"], "sk-kimi-test")

    def test_validate_provider_key_rejects_invalid_kimi_key_with_upstream_detail(self):
        http_error = WEB_SERVER.urllib.error.HTTPError(
            url=WEB_SERVER.KIMI_VALIDATE_URL,
            code=401,
            msg="Unauthorized",
            hdrs=None,
            fp=io.BytesIO(json.dumps({
                "error": {
                    "message": "Invalid Authentication",
                    "type": "invalid_authentication_error",
                }
            }).encode("utf-8")),
        )

        with mock.patch.object(WEB_SERVER.urllib.request, "urlopen", side_effect=http_error):
            ok, message = WEB_SERVER.validate_provider_key("kimi", "sk-kimi-test")

        self.assertFalse(ok)
        self.assertEqual(
            message,
            "Kimi rejected this API key: Invalid Authentication (invalid_authentication_error)",
        )

    def test_clear_key_removes_saved_provider(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            keys_file = Path(tmpdir) / "api-keys.json"
            keys_file.write_text(json.dumps({"kimi": "sk-kimi", "openai": "sk-openai"}))

            with mock.patch.object(WEB_SERVER, "KEYS_FILE", keys_file):
                removed = WEB_SERVER.clear_key("kimi")
                remaining = json.loads(keys_file.read_text())

        self.assertTrue(removed)
        self.assertEqual(remaining, {"openai": "sk-openai"})

    def test_clear_key_deletes_file_when_last_key_removed(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            keys_file = Path(tmpdir) / "api-keys.json"
            keys_file.write_text(json.dumps({"kimi": "sk-kimi"}))

            with mock.patch.object(WEB_SERVER, "KEYS_FILE", keys_file):
                removed = WEB_SERVER.clear_key("kimi")

        self.assertTrue(removed)
        self.assertFalse(keys_file.exists())


class WebServerPetdexTests(unittest.TestCase):
    def test_fetch_petdex_follows_redirects_and_maps_results(self):
        upstream_payload = {
            "total": 321,
            "nextCursor": 8,
            "pets": [
                {
                    "slug": "mallow",
                    "displayName": "Mallow",
                    "description": "Cat mascot",
                    "spritesheetPath": "https://assets.petdex.dev/pets/mallow/sprite.webp",
                    "dominantColor": "#cf9f33",
                    "vibes": ["calm", "focused", "wholesome", "cozy"],
                }
            ],
        }
        completed = subprocess_result = mock.Mock(
            returncode=0,
            stdout=json.dumps(upstream_payload),
            stderr="",
        )

        with mock.patch.object(WEB_SERVER.subprocess, "run", return_value=completed) as run_mock:
            result = WEB_SERVER.fetch_petdex("cat", 8, 0)

        cmd = run_mock.call_args.args[0]
        self.assertIn("-L", cmd)
        self.assertIn("--fail", cmd)
        self.assertEqual(result["query"], "cat")
        self.assertEqual(result["total"], 321)
        self.assertEqual(result["nextCursor"], 8)
        self.assertEqual(len(result["pets"]), 1)
        self.assertEqual(result["pets"][0]["slug"], "mallow")
        self.assertEqual(result["pets"][0]["displayName"], "Mallow")
        self.assertEqual(result["pets"][0]["vibes"], ["calm", "focused", "wholesome"])


if __name__ == "__main__":
    unittest.main()
