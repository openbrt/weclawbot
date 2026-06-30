"""WeClawBot plugin for Hermes.

The device owns its hardware limits.  This plugin only binds an already
displayed six-digit code and tells a paired screen when Hermes is actually
working.  It does not receive a WeChat token, Wi-Fi credential, or model key.
"""

from __future__ import annotations

import logging
import os
import threading
import uuid
from typing import Any

from .cli import register_cli, weclawbot_command
from .mqtt_control import WeClawBotError, publish_activity
from .tools import (
    ACTIVITY_SCHEMA,
    CLEAR_SCHEMA,
    SCREEN_DOCUMENT_SCHEMA,
    handle_activity,
    handle_clear_screen,
    handle_screen_document,
)

logger = logging.getLogger(__name__)
_lock = threading.Lock()
_pending: dict[str, list[str]] = {}


def _enabled() -> bool:
    return os.getenv("WEC_HERMES_ACTIVITY_AUTO", "1").strip().lower() not in {
        "0", "false", "no", "off",
    }


def _key(kwargs: dict[str, Any]) -> str:
    for name in ("request_id", "turn_id", "task_id", "session_id"):
        value = kwargs.get(name)
        if value:
            return str(value)
    return "default"


def _pre_llm_call(**kwargs: Any) -> None:
    """Show the pet only while Hermes has begun an actual model call."""
    if not _enabled():
        return
    correlation_id = f"hermes-{uuid.uuid4().hex}"
    try:
        publish_activity("thinking", correlation_id, ttl_seconds=120)
    except WeClawBotError as error:
        # Pairing is optional. A missing/offline display must never slow down
        # or break a normal Hermes turn.
        logger.debug("weclawbot thinking activity skipped: %s", error)
        return
    with _lock:
        _pending.setdefault(_key(kwargs), []).append(correlation_id)


def _post_llm_call(**kwargs: Any) -> None:
    if not _enabled():
        return
    with _lock:
        values = _pending.get(_key(kwargs), [])
        correlation_id = values.pop() if values else ""
        if not values:
            _pending.pop(_key(kwargs), None)
    if not correlation_id:
        return
    try:
        publish_activity("idle", correlation_id)
    except WeClawBotError as error:
        logger.debug("weclawbot idle activity skipped: %s", error)


def register(ctx) -> None:
    ctx.register_tool(
        name="weclawbot_activity",
        toolset="weclawbot",
        schema=ACTIVITY_SCHEMA,
        handler=handle_activity,
        description="Temporarily show or clear the paired WeClawBot thinking pet.",
    )
    ctx.register_tool(
        name="weclawbot_screen_document",
        toolset="weclawbot",
        schema=SCREEN_DOCUMENT_SCHEMA,
        handler=handle_screen_document,
        description="Put a bounded 1-bit document on the paired WeClawBot screen.",
    )
    ctx.register_tool(
        name="weclawbot_clear_screen",
        toolset="weclawbot",
        schema=CLEAR_SCHEMA,
        handler=handle_clear_screen,
        description="Clear the paired WeClawBot note or idle photo with firmware screen_clear.",
    )
    ctx.register_cli_command(
        name="weclawbot",
        help="Bind and inspect a paired WeClawBot screen",
        setup_fn=register_cli,
        handler_fn=weclawbot_command,
        description="Bind a WeClawBot six-digit code without entering an endpoint URL.",
    )
    ctx.register_hook("pre_llm_call", _pre_llm_call)
    ctx.register_hook("post_llm_call", _post_llm_call)
