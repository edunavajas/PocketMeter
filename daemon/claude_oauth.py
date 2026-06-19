import json
import os
import tempfile
import time
import urllib.request
from pathlib import Path


CLAUDE_OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
CLAUDE_OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
CLAUDE_OAUTH_REFRESH_SKEW_MS = 5 * 60 * 1000


def load_claude_credentials(credentials_path):
    credentials = json.loads(Path(credentials_path).read_text())
    oauth = credentials.get("claudeAiOauth", {})
    if not isinstance(oauth, dict):
        raise RuntimeError("Invalid claudeAiOauth credentials")
    return credentials, oauth


def expires_soon(expires_at, *, now_ms=None, skew_ms=CLAUDE_OAUTH_REFRESH_SKEW_MS):
    if expires_at in (None, ""):
        return False
    try:
        expires_at = int(expires_at)
    except (TypeError, ValueError):
        return False
    if expires_at <= 0:
        return False
    if expires_at < 1_000_000_000_000:
        expires_at *= 1000
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    return expires_at <= now_ms + skew_ms


def _atomic_write_json(path, payload):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_path = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_path, path)
    except Exception:
        try:
            os.unlink(temp_path)
        except OSError:
            pass
        raise


def refresh_access_token(credentials_path, *, timeout=10):
    credentials_path = Path(credentials_path)
    credentials, oauth = load_claude_credentials(credentials_path)
    refresh_token = oauth.get("refreshToken")
    if not refresh_token:
        raise RuntimeError("No refreshToken in claudeAiOauth")

    payload = json.dumps({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": CLAUDE_OAUTH_CLIENT_ID,
    }).encode("utf-8")
    request = urllib.request.Request(
        CLAUDE_OAUTH_TOKEN_URL,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "claude-code/2.1.5",
        },
        method="POST",
    )

    with urllib.request.urlopen(request, timeout=timeout) as response:
        refreshed = json.loads(response.read().decode("utf-8"))

    access_token = refreshed.get("access_token")
    expires_in = refreshed.get("expires_in")
    if not access_token:
        raise RuntimeError("Claude OAuth refresh did not return access_token")
    try:
        expires_at = int((time.time() + float(expires_in)) * 1000)
    except (TypeError, ValueError):
        raise RuntimeError("Claude OAuth refresh did not return a valid expires_in") from None

    updated_oauth = dict(oauth)
    updated_oauth["accessToken"] = access_token
    updated_oauth["expiresAt"] = expires_at
    if refreshed.get("refresh_token"):
        updated_oauth["refreshToken"] = refreshed["refresh_token"]

    updated_credentials = dict(credentials)
    updated_credentials["claudeAiOauth"] = updated_oauth
    _atomic_write_json(credentials_path, updated_credentials)
    return updated_oauth


def get_access_token(credentials_path, *, force_refresh=False, timeout=10):
    credentials_path = Path(credentials_path)
    _, oauth = load_claude_credentials(credentials_path)
    access_token = oauth.get("accessToken")
    should_refresh = force_refresh or not access_token or expires_soon(oauth.get("expiresAt"))
    if should_refresh:
        oauth = refresh_access_token(credentials_path, timeout=timeout)
        access_token = oauth.get("accessToken")
    if not access_token:
        raise RuntimeError("No accessToken in claudeAiOauth")
    return access_token, oauth
