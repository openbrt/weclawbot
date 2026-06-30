"""Small dependency-free MQTT 3.1.1 publisher over Hermes' WebSocket stack."""

from __future__ import annotations

import base64
import json
import os
import secrets
import struct
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from websockets.sync.client import connect

DEFAULT_ENDPOINT = "https://weclawbot.link/byoa"
DEFAULT_CREDENTIAL_PATH = Path.home() / ".config" / "weclawbot" / "agent-mqtt.json"


class WeClawBotError(RuntimeError):
    """A concise pairing or live-delivery error safe to show to an operator."""


def credentials_path() -> Path:
    raw = os.getenv("WEC_AGENT_CREDENTIALS_PATH", "").strip()
    return Path(raw).expanduser() if raw else DEFAULT_CREDENTIAL_PATH


def bind_pairing(code: str, agent_name: str = "Hermes") -> dict[str, Any]:
    digits = "".join(char for char in str(code) if char.isdigit())
    if len(digits) != 6:
        raise WeClawBotError("请输入屏幕上的六码绑定码")
    payload = _request_json({
        "schema": "weclawbot.byoa.v1",
        "operation": "claim",
        "code": digits,
        "agent_name": str(agent_name or "Hermes")[:80],
    })
    if payload.get("schema") != "weclawbot.byoa.agent_credentials.v1" or not payload.get("mqtt"):
        raise WeClawBotError("绑定服务返回的数据不完整")
    path = credentials_path()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "schema": "weclawbot.agent_credentials.v1",
        "binding": payload.get("binding", {}),
        "mqtt": payload["mqtt"],
        "delivery": payload.get("delivery", {}),
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)
    return payload["binding"]


def binding_status() -> dict[str, Any] | None:
    path = credentials_path()
    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise WeClawBotError("本地绑定凭据无法读取") from error
    return payload if isinstance(payload, dict) else None


def forget_binding() -> Path:
    path = credentials_path()
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    except OSError as error:
        raise WeClawBotError("无法删除本地绑定凭据") from error
    return path


def doctor_status(test_online: bool = False) -> dict[str, Any]:
    path = credentials_path()
    checks: list[dict[str, Any]] = []
    payload = binding_status()
    checks.append({"name": "credentials_file", "ok": bool(payload), "detail": str(path)})
    if not payload:
        return {"ok": False, "checks": checks}
    try:
        mode = path.stat().st_mode & 0o777
        checks.append({"name": "credentials_permissions", "ok": mode & 0o077 == 0, "detail": oct(mode)})
    except OSError as error:
        checks.append({"name": "credentials_permissions", "ok": False, "detail": str(error)})
    try:
        mqtt = _normalized_mqtt(payload)
        checks.append({"name": "mqtt_profile", "ok": True, "detail": mqtt["client_id"]})
        checks.append({"name": "mqtt_url_tls", "ok": mqtt["url"].startswith("wss://"), "detail": mqtt["url"]})
        checks.append({
            "name": "client_id_contains_no_secret",
            "ok": not any(token in mqtt["client_id"].lower() for token in ("token", "secret", "password", "passwd", "key")),
            "detail": mqtt["client_id"],
        })
        if test_online:
            try:
                _test_mqtt_connection(mqtt)
                checks.append({"name": "mqtt_online", "ok": True, "detail": "connected"})
            except WeClawBotError as error:
                checks.append({"name": "mqtt_online", "ok": False, "detail": str(error)})
    except WeClawBotError as error:
        checks.append({"name": "mqtt_profile", "ok": False, "detail": str(error)})
    return {"ok": all(check["ok"] for check in checks), "checks": checks}


def export_profile(fmt: str = "env", include_secret: bool = False) -> str:
    payload = binding_status()
    if not payload:
        raise WeClawBotError("尚未绑定微笺屏")
    mqtt = _normalized_mqtt(payload)
    password = mqtt["password"] if include_secret else "********"
    if fmt == "json":
        data = json.loads(json.dumps(payload))
        if not include_secret and data.get("mqtt", {}).get("password"):
            data["mqtt"]["password"] = "********"
        return json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    if fmt == "env":
        return "\n".join([
            f"WEC_MQTT_URL={_shell_value(mqtt['url'])}",
            f"WEC_MQTT_CLIENT_ID={_shell_value(mqtt['client_id'])}",
            f"WEC_MQTT_USERNAME={_shell_value(mqtt['username'])}",
            f"WEC_MQTT_PASSWORD={_shell_value(password)}",
            f"WEC_MQTT_CONTROL_TOPIC={_shell_value(mqtt['control_topic'])}",
            "",
        ])
    if fmt == "mosquitto":
        return "\n".join([
            f"url {mqtt['url']}",
            f"id {mqtt['client_id']}",
            f"username {mqtt['username']}",
            f"password {password}",
            "protocol-version mqttv5",
            "clean-session true",
            "",
        ])
    raise WeClawBotError("导出格式必须是 env、json 或 mosquitto")


def publish_activity(state: str, correlation_id: str, ttl_seconds: int | None = None) -> None:
    if state not in {"thinking", "idle"}:
        raise WeClawBotError("活动状态必须是 thinking 或 idle")
    if not correlation_id:
        raise WeClawBotError("活动状态缺少关联编号")
    if state == "thinking" and (not isinstance(ttl_seconds, int) or not 5 <= ttl_seconds <= 120):
        raise WeClawBotError("思考状态时长必须在 5 到 120 秒之间")
    control = {
        "schema": "weclawbot.control.v1",
        "id": f"activity_{secrets.token_hex(12)}",
        "kind": "activity",
        "activity": {
            "schema": "weclawbot.activity.v1",
            "state": state,
            "correlation_id": correlation_id,
        },
    }
    if state == "thinking":
        control["activity"]["ttl_seconds"] = ttl_seconds
    publish_control(control)


def publish_screen_document(document: dict[str, Any]) -> None:
    """Send a bounded, pre-rendered mono1 document to the paired screen."""
    _validate_screen_document(document)
    publish_control({
        "schema": "weclawbot.control.v1",
        "id": f"screen_{secrets.token_hex(12)}",
        "kind": "screen_document",
        "document": document,
    })


def publish_screen_clear(target: str = "note") -> None:
    normalized = _normalize_clear_target(target)
    publish_control({
        "schema": "weclawbot.control.v1",
        "id": f"clear_{secrets.token_hex(12)}",
        "kind": "screen_clear",
        "target": normalized,
    })


def publish_control(control: dict[str, Any]) -> None:
    stored = binding_status()
    if not stored:
        raise WeClawBotError("尚未绑定微笺屏")
    mqtt = stored.get("mqtt") if isinstance(stored.get("mqtt"), dict) else {}
    _publish_mqtt(mqtt, control)


def _validate_screen_document(document: dict[str, Any]) -> None:
    if not isinstance(document, dict) or document.get("schema") != "weclawbot.screen_document.v1":
        raise WeClawBotError("屏幕文档格式不正确")
    if document.get("target") != "content" or document.get("kind") != "replace":
        raise WeClawBotError("屏幕文档只能替换内容区")
    document_id = document.get("id")
    if not isinstance(document_id, str) or not document_id or len(document_id) > 80:
        raise WeClawBotError("屏幕文档缺少有效编号")
    if not isinstance(document.get("base_revision"), str):
        raise WeClawBotError("屏幕文档缺少当前版本")
    expires_at = document.get("expires_at")
    if not isinstance(expires_at, str):
        raise WeClawBotError("屏幕文档缺少过期时间")
    try:
        expiry = datetime.fromisoformat(expires_at.replace("Z", "+00:00"))
    except ValueError as error:
        raise WeClawBotError("屏幕文档过期时间无效") from error
    if expiry.tzinfo is None or expiry <= datetime.now(timezone.utc):
        raise WeClawBotError("屏幕文档已经过期")
    pages = document.get("pages")
    if not isinstance(pages, list) or not 1 <= len(pages) <= 3:
        raise WeClawBotError("屏幕文档只能包含一到三页")
    geometry: tuple[int, int, int] | None = None
    uniform_pages = 0
    for page in pages:
        if not isinstance(page, dict) or page.get("format") != "mono1":
            raise WeClawBotError("屏幕页面必须是 mono1")
        width, height, stride = page.get("width"), page.get("height"), page.get("stride")
        if not all(isinstance(value, int) for value in (width, height, stride)) or not (
            1 <= width <= 368 and 1 <= height <= 206 and stride >= (width + 7) // 8
        ):
            raise WeClawBotError("屏幕页面尺寸超出内容区")
        try:
            data = base64.b64decode(page.get("data_b64", ""), validate=True)
        except (TypeError, ValueError) as error:
            raise WeClawBotError("屏幕页面位图不是有效 Base64") from error
        if len(data) != stride * height:
            raise WeClawBotError("屏幕页面位图长度不匹配")
        if data and all(byte == data[0] for byte in data):
            uniform_pages += 1
        current_geometry = (width, height, stride)
        if geometry is None:
            geometry = current_geometry
        elif geometry != current_geometry:
            raise WeClawBotError("所有屏幕页面必须使用同一尺寸")
    if uniform_pages == len(pages):
        raise WeClawBotError("纯色屏幕文档不是清屏；请使用 screen_clear")


def _normalize_clear_target(target: str) -> str:
    value = str(target or "note").strip()
    if value == "photo":
        return "idle_photo"
    if value in {"note", "idle_photo"}:
        return value
    raise WeClawBotError("清屏目标只能是 note 或 idle_photo")


def _request_json(body: dict[str, Any]) -> dict[str, Any]:
    endpoint = os.getenv("WEC_BYOA_ENDPOINT", DEFAULT_ENDPOINT).strip() or DEFAULT_ENDPOINT
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(body).encode("utf-8"),
        headers={"content-type": "application/json", "accept": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        try:
            detail = json.loads(error.read().decode("utf-8")).get("error")
        except Exception:
            detail = None
        raise WeClawBotError(str(detail or f"绑定失败（HTTP {error.code}）")) from error
    except (OSError, ValueError) as error:
        raise WeClawBotError("无法连接绑定服务") from error
    if not isinstance(payload, dict) or not payload.get("ok"):
        raise WeClawBotError(str(payload.get("error") if isinstance(payload, dict) else "绑定失败"))
    return payload


def _mqtt_string(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack("!H", len(raw)) + raw


def _remaining_length(length: int) -> bytes:
    result = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length:
            digit |= 0x80
        result.append(digit)
        if not length:
            return bytes(result)


def _packet(header: int, body: bytes) -> bytes:
    return bytes([header]) + _remaining_length(len(body)) + body


def _connect_packet(client_id: str, username: str, password: str) -> bytes:
    flags = 0xC2  # username, password, clean session
    body = _mqtt_string("MQTT") + bytes([4, flags]) + struct.pack("!H", 20)
    body += _mqtt_string(client_id) + _mqtt_string(username) + _mqtt_string(password)
    return _packet(0x10, body)


def _publish_packet(topic: str, payload: bytes) -> bytes:
    message_id = secrets.randbelow(65535) + 1
    return _packet(0x32, _mqtt_string(topic) + struct.pack("!H", message_id) + payload)


def _as_bytes(value: Any) -> bytes:
    return value.encode("utf-8") if isinstance(value, str) else bytes(value)


def _publish_mqtt(mqtt: dict[str, Any], control: dict[str, Any]) -> None:
    profile = _normalized_mqtt({"mqtt": mqtt})
    try:
        with connect(profile["url"], subprotocols=["mqtt"], open_timeout=6, close_timeout=1) as socket:
            socket.send(_connect_packet(profile["client_id"], profile["username"], profile["password"]))
            connack = _as_bytes(socket.recv(timeout=6))
            if len(connack) < 4 or connack[0] != 0x20 or connack[3] != 0:
                raise WeClawBotError("屏幕消息通道认证失败")
            socket.send(_publish_packet(profile["control_topic"], json.dumps(control, separators=(",", ":")).encode("utf-8")))
            puback = _as_bytes(socket.recv(timeout=6))
            if not puback or puback[0] != 0x40:
                raise WeClawBotError("屏幕没有确认消息")
    except WeClawBotError:
        raise
    except Exception as error:
        raise WeClawBotError("屏幕当前不在线或消息通道不可用") from error


def _normalized_mqtt(payload: dict[str, Any]) -> dict[str, str]:
    mqtt = payload.get("mqtt") if isinstance(payload.get("mqtt"), dict) else payload
    url = str(mqtt.get("url") or "")
    username = str(mqtt.get("username") or "")
    password = str(mqtt.get("password") or "")
    client_id = str(mqtt.get("client_id") or "")
    topics = mqtt.get("topics") if isinstance(mqtt.get("topics"), dict) else {}
    topic = str(topics.get("control") or "")
    if not url.startswith("wss://") or not all((username, password, client_id, topic)):
        raise WeClawBotError("本地绑定凭据不完整")
    return {"url": url, "username": username, "password": password, "client_id": client_id, "control_topic": topic}


def _test_mqtt_connection(profile: dict[str, str]) -> None:
    try:
        with connect(profile["url"], subprotocols=["mqtt"], open_timeout=6, close_timeout=1) as socket:
            socket.send(_connect_packet(profile["client_id"], profile["username"], profile["password"]))
            connack = _as_bytes(socket.recv(timeout=6))
            if len(connack) < 4 or connack[0] != 0x20 or connack[3] != 0:
                raise WeClawBotError("屏幕消息通道认证失败")
    except WeClawBotError:
        raise
    except Exception as error:
        raise WeClawBotError("屏幕当前不在线或消息通道不可用") from error


def _shell_value(value: str) -> str:
    return "'" + str(value).replace("'", "'\\''") + "'"
