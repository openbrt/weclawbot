from __future__ import annotations

import json
import uuid
from typing import Any

from .mqtt_control import WeClawBotError, publish_activity, publish_screen_clear, publish_screen_document

ACTIVITY_SCHEMA = {
    "name": "weclawbot_activity",
    "description": "Temporarily show the paired WeClawBot thinking pet, then restore the prior screen.",
    "parameters": {
        "type": "object",
        "properties": {
            "state": {"type": "string", "enum": ["thinking", "idle"]},
            "ttl_seconds": {"type": "integer", "minimum": 5, "maximum": 120},
            "correlation_id": {"type": "string"},
        },
        "required": ["state"],
        "additionalProperties": False,
    },
}

SCREEN_DOCUMENT_SCHEMA = {
    "name": "weclawbot_screen_document",
    "description": "Put a validated 1-bit, one-to-three page document in the paired WeClawBot content area.",
    "parameters": {
        "type": "object",
        "properties": {"document": {"type": "object"}},
        "required": ["document"],
        "additionalProperties": False,
    },
}

CLEAR_SCHEMA = {
    "name": "weclawbot_clear_screen",
    "description": "Clear the paired WeClawBot note or idle-photo state. Do not emulate clearing with a blank or black bitmap.",
    "parameters": {
        "type": "object",
        "properties": {"target": {"type": "string", "enum": ["note", "idle_photo", "photo"]}},
        "additionalProperties": False,
    },
}


def handle_activity(args: dict[str, Any], **_: Any) -> str:
    state = str(args.get("state") or "")
    correlation_id = str(args.get("correlation_id") or f"hermes-{uuid.uuid4().hex}")
    try:
        publish_activity(state, correlation_id, args.get("ttl_seconds", 45) if state == "thinking" else None)
    except WeClawBotError as error:
        return json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False)
    return json.dumps({"ok": True, "state": state, "correlation_id": correlation_id}, ensure_ascii=False)


def handle_screen_document(args: dict[str, Any], **_: Any) -> str:
    document = args.get("document")
    try:
        publish_screen_document(document)
    except WeClawBotError as error:
        return json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False)
    return json.dumps({"ok": True, "id": document.get("id"), "pages": len(document.get("pages", []))}, ensure_ascii=False)


def handle_clear_screen(args: dict[str, Any], **_: Any) -> str:
    target = str(args.get("target") or "note")
    try:
        publish_screen_clear(target)
    except WeClawBotError as error:
        return json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False)
    return json.dumps({"ok": True, "target": "idle_photo" if target == "photo" else target}, ensure_ascii=False)
