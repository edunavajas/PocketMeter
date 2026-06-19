import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "daemon" / "pocketmeter-daemon.py"
DAEMON_DIR = MODULE_PATH.parent
if str(DAEMON_DIR) not in sys.path:
    sys.path.insert(0, str(DAEMON_DIR))
SPEC = importlib.util.spec_from_file_location("pocketmeter_daemon", MODULE_PATH)
DAEMON = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(DAEMON)


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def read(self):
        return json.dumps(self.payload).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


class FakeClaudeResponse:
    def __init__(self, headers):
        self.headers = headers

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


class PocketMeterDaemonTests(unittest.TestCase):
    def test_claude_provider_refreshes_and_retries_after_401(self):
        provider = DAEMON.ClaudeProvider()
        headers = {
            "anthropic-ratelimit-unified-5h-utilization": "0.25",
            "anthropic-ratelimit-unified-5h-reset": "4600",
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": "8200",
            "anthropic-ratelimit-unified-5h-status": "ok",
        }
        http_error = DAEMON.urllib.error.HTTPError(
            url="https://api.anthropic.com/v1/messages",
            code=401,
            msg="Unauthorized",
            hdrs=None,
            fp=None,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            creds_file = Path(tmpdir) / "credentials.json"
            creds_file.write_text(json.dumps({
                "claudeAiOauth": {
                    "accessToken": "expired-token",
                    "refreshToken": "refresh-token",
                    "expiresAt": 9_999_999_999_999,
                    "subscriptionType": "max",
                }
            }))
            provider.credentials_file = creds_file

            refresh_response = FakeResponse({
                "access_token": "fresh-token",
                "refresh_token": "rotated-refresh-token",
                "expires_in": 3600,
            })

            def fake_urlopen(req, timeout=10):
                auth_header = req.get_header("Authorization")
                if req.full_url == DAEMON.ClaudeProvider._API_URL and auth_header == "Bearer expired-token":
                    raise http_error
                if req.full_url == DAEMON.ClaudeProvider._API_URL and auth_header == "Bearer fresh-token":
                    return FakeClaudeResponse(headers)
                if req.full_url == "https://platform.claude.com/v1/oauth/token":
                    return refresh_response
                raise AssertionError(f"Unexpected request: {req.full_url} {auth_header}")

            with mock.patch.object(DAEMON.time, "time", return_value=1000):
                with mock.patch.object(DAEMON.urllib.request, "urlopen", side_effect=fake_urlopen) as mocked_urlopen:
                    result = provider.fetch_usage()

            persisted = json.loads(creds_file.read_text())

        self.assertEqual(mocked_urlopen.call_count, 3)
        self.assertFalse(result.get("simulated", False))
        self.assertEqual(result["provider"], "claude")
        self.assertEqual(result["plan_type"], "max")
        self.assertEqual(result["session_pct"], 25.0)
        self.assertEqual(result["weekly_pct"], 50.0)
        self.assertEqual(persisted["claudeAiOauth"]["accessToken"], "fresh-token")
        self.assertEqual(persisted["claudeAiOauth"]["refreshToken"], "rotated-refresh-token")

    def test_claude_provider_check_available_accepts_refresh_only(self):
        provider = DAEMON.ClaudeProvider()

        with tempfile.TemporaryDirectory() as tmpdir:
            creds_file = Path(tmpdir) / "credentials.json"
            creds_file.write_text(json.dumps({
                "claudeAiOauth": {
                    "refreshToken": "refresh-only",
                }
            }))
            provider.credentials_file = creds_file

            self.assertTrue(provider.check_available())

    def test_get_api_key_normalizes_bearer_and_quotes(self):
        with mock.patch.dict(DAEMON.os.environ, {"KIMI_API_KEY": "  'Bearer sk-kimi-test'  "}, clear=False):
            key = DAEMON._get_api_key("KIMI_API_KEY", provider_id="kimi")

        self.assertEqual(key, "sk-kimi-test")

    def test_codex_provider_is_available_with_refresh_token_only(self):
        provider = DAEMON.CodexProvider()
        auth = {
            "tokens": {
                "refresh_token": "refresh-only",
            }
        }

        with tempfile.TemporaryDirectory() as tmpdir:
            auth_file = Path(tmpdir) / "auth.json"
            auth_file.write_text(json.dumps(auth))
            provider.auth_file = auth_file

            self.assertTrue(provider.check_available())

    def test_codex_provider_uses_recent_cached_result_during_transient_401(self):
        provider = DAEMON.CodexProvider()
        provider._last_ok_result = {
            "provider": "codex",
            "session_pct": 10,
            "weekly_pct": 20,
            "ok": True,
        }
        provider._last_ok_at = 1000

        http_error = DAEMON.urllib.error.HTTPError(
            url="https://chatgpt.com/backend-api/wham/usage",
            code=401,
            msg="Unauthorized",
            hdrs=None,
            fp=None,
        )

        with mock.patch.object(DAEMON.time, "time", return_value=1060):
            with mock.patch.object(provider, "_load_auth", return_value={"tokens": {"access_token": "token"}}):
                with mock.patch.object(DAEMON.urllib.request, "urlopen", side_effect=http_error):
                    result = provider.fetch_usage()

        self.assertIsNotNone(result)
        self.assertTrue(result["ok"])
        self.assertTrue(result["stale"])
        self.assertEqual(result["stale_reason"], "Token expired")

    def test_validate_esp_host_accepts_status_payload(self):
        payload = {
            "wifi_connected": True,
            "ip": "192.168.1.77",
            "hostname": "pocketmeter",
        }
        with mock.patch.object(DAEMON.urllib.request, "urlopen", return_value=FakeResponse(payload)):
            endpoint = DAEMON.validate_esp_host("pocketmeter.local", "hostname")

        self.assertIsNotNone(endpoint)
        self.assertEqual(endpoint.host, "pocketmeter.local")
        self.assertEqual(endpoint.ip, "192.168.1.77")
        self.assertEqual(endpoint.hostname, "pocketmeter")

    def test_discovery_candidates_prioritize_override_then_cache(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cache_file = Path(tmpdir) / "esp-ip"
            cache_file.write_text("192.168.1.44\n")

            with mock.patch.object(DAEMON, "ESP_IP_FILE", cache_file):
                with mock.patch.dict(DAEMON.os.environ, {DAEMON.ESP_HOST_OVERRIDE: "override.local"}, clear=False):
                    candidates = DAEMON.discovery_candidates()

        self.assertEqual(candidates[0], ("override.local", f"env:{DAEMON.ESP_HOST_OVERRIDE}"))
        self.assertEqual(candidates[1], ("192.168.1.44", "cache"))

    def test_discover_esp_endpoint_replaces_stale_cache(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cache_file = Path(tmpdir) / "esp-ip"
            cache_file.write_text("192.168.1.10\n")

            def fake_validate(host, source, timeout=DAEMON.HTTP_TIMEOUT):
                if host == "192.168.1.10":
                    return None
                if host == "pocketmeter.local":
                    return DAEMON.EspEndpoint(host=host, ip="192.168.1.88", source=source, hostname="pocketmeter")
                return None

            with mock.patch.object(DAEMON, "ESP_IP_FILE", cache_file):
                with mock.patch.object(DAEMON, "validate_esp_host", side_effect=fake_validate):
                    endpoint = DAEMON.discover_esp_endpoint()

            self.assertIsNotNone(endpoint)
            self.assertEqual(endpoint.ip, "192.168.1.88")
            self.assertEqual(cache_file.read_text().strip(), "192.168.1.88")

    def test_discovery_subnets_include_seed_and_local_networks(self):
        with mock.patch.object(DAEMON, "resolve_local_ipv4_addresses", return_value=["192.168.1.23", "10.0.0.9"]):
            networks = DAEMON.discovery_subnets("192.168.50.12")

        self.assertEqual(
            [str(network) for network in networks],
            ["192.168.50.0/24", "192.168.1.0/24", "10.0.0.0/24"],
        )

    def test_refresh_cached_ip_keeps_hostname_override(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cache_file = Path(tmpdir) / "esp-ip"
            cache_file.write_text("pocketmeter.local\n")
            endpoint = DAEMON.EspEndpoint(host="pocketmeter.local", ip="192.168.1.88", source="hostname")

            with mock.patch.object(DAEMON, "ESP_IP_FILE", cache_file):
                DAEMON.refresh_cached_ip("pocketmeter.local", endpoint)

            self.assertEqual(cache_file.read_text().strip(), "pocketmeter.local")

    def test_kimi_provider_uses_kimi_coding_models_endpoint(self):
        provider = DAEMON.KimiProvider()
        payload = {
            "object": "list",
            "data": [{"id": "kimi-k2"}],
        }

        def fake_urlopen(req, timeout=10):
            self.assertEqual(req.full_url, "https://api.kimi.com/coding/v1/models")
            self.assertEqual(req.get_header("Authorization"), "Bearer test-kimi-key")
            return FakeResponse(payload)

        with mock.patch.object(DAEMON, "_get_api_key", return_value="test-kimi-key"):
            with mock.patch.object(DAEMON.urllib.request, "urlopen", side_effect=fake_urlopen):
                result = provider.fetch_usage()

        self.assertEqual(result["provider"], "kimi")
        self.assertEqual(result["plan_type"], "api")
        self.assertFalse(result["has_credits"])
        self.assertFalse(result["metrics_available"])
        self.assertEqual(result["note"], "No usage metrics via Kimi Coding API")
        self.assertTrue(result["ok"])

    def test_kimi_provider_accepts_models_list_payload(self):
        provider = DAEMON.KimiProvider()
        payload = {
            "data": [
                {"id": "kimi-k2"},
                {"id": "kimi-k1.5"},
            ]
        }

        with mock.patch.object(DAEMON, "_get_api_key", return_value="test-kimi-key"):
            with mock.patch.object(DAEMON.urllib.request, "urlopen", return_value=FakeResponse(payload)):
                result = provider.fetch_usage()

        self.assertEqual(result["plan_type"], "api")
        self.assertFalse(result["has_credits"])

    def test_kimi_provider_surfaces_upstream_invalid_auth_reason(self):
        provider = DAEMON.KimiProvider()

        http_error = DAEMON.urllib.error.HTTPError(
            url=provider._MODELS_URL,
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

        with mock.patch.object(DAEMON, "_get_api_key", return_value="test-kimi-key"):
            with mock.patch.object(DAEMON.urllib.request, "urlopen", side_effect=http_error):
                result = provider.fetch_usage()

        self.assertIsNone(result)
        self.assertEqual(
            provider.last_error,
            "Invalid API key (Invalid Authentication (invalid_authentication_error))",
        )

    def test_merge_provider_data_marks_configured_fetch_failures(self):
        class ConfiguredButFailingProvider:
            name = "kimi"

            def __init__(self):
                self.available = True
                self.last_error = "Invalid API key"

            def check_available(self):
                return True

            def fetch_usage(self):
                return None

        data = DAEMON.merge_provider_data([ConfiguredButFailingProvider()])

        self.assertEqual(data["providers"][0]["provider"], "kimi")
        self.assertTrue(data["providers"][0]["configured"])
        self.assertFalse(data["providers"][0]["ok"])


if __name__ == "__main__":
    unittest.main()
