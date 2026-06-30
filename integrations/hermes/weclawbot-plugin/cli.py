from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from .mqtt_control import (
    WeClawBotError,
    bind_pairing,
    binding_status,
    credentials_path,
    doctor_status,
    export_profile,
    forget_binding,
    publish_activity,
    publish_screen_clear,
    publish_screen_document,
)


def register_cli(subparser: argparse.ArgumentParser) -> None:
    commands = subparser.add_subparsers(dest="weclawbot_action")
    bind = commands.add_parser("bind", help="Bind the six-digit code shown on a WeClawBot screen")
    bind.add_argument("code", help="Six-digit code shown by the screen")
    bind.add_argument("--name", default="Hermes", help="Name shown to the local pairing record")
    status = commands.add_parser("status", help="Show whether this Hermes installation is paired")
    status.add_argument("--json", action="store_true", help="Print machine-readable status")
    legacy_status = commands.add_parser("agent-status", help=argparse.SUPPRESS)
    legacy_status.add_argument("--json", action="store_true", help=argparse.SUPPRESS)
    doctor = commands.add_parser("doctor", help="Validate the local MQTT profile")
    doctor.add_argument("--online", action="store_true", help="Open a live MQTT/WebSocket connection")
    doctor.add_argument("--json", action="store_true", help="Print machine-readable diagnostics")
    export = commands.add_parser("export", help="Export the local MQTT profile for another tool")
    export.add_argument("--format", choices=["env", "json", "mosquitto"], default="env")
    export.add_argument("--include-secret", action="store_true", help="Include the MQTT password")
    export.add_argument("--output", help="Write the profile to a file")
    unbind = commands.add_parser("unbind", help="Remove the local MQTT credential")
    unbind.add_argument("--yes", action="store_true", help="Confirm credential deletion")
    thinking = commands.add_parser("thinking", help="Show the thinking activity state")
    thinking.add_argument("--id", required=True, help="Correlation id for the activity")
    thinking.add_argument("--ttl", type=int, default=45, help="Thinking timeout in seconds")
    idle = commands.add_parser("idle", help="Return the screen to idle activity state")
    idle.add_argument("--id", required=True, help="Correlation id for the activity")
    screen = commands.add_parser("screen", help="Send a pre-rendered 1-bit screen document")
    screen.add_argument("document", help="Path to a screen_document JSON file")
    clear = commands.add_parser("clear", help="Clear the current note or idle photo")
    clear.add_argument("--target", choices=["note", "idle_photo", "photo"], default="note")


def weclawbot_command(args: argparse.Namespace) -> int:
    try:
        if args.weclawbot_action == "bind":
            binding = bind_pairing(args.code, args.name)
            print(f"WeClawBot 已绑定：{binding.get('device_id', 'device')}。后续 Hermes 思考会显示在屏上。")
            return 0
        if args.weclawbot_action in {"status", "agent-status"}:
            payload = binding_status()
            if getattr(args, "json", False):
                print(json.dumps(_status_payload(payload), ensure_ascii=False, indent=2))
                return 0 if payload else 1
            if not payload:
                print("WeClawBot 未绑定。请先在屏幕上选择自定义智能体，再运行 hermes weclawbot bind <六码>。")
                return 1
            binding = payload.get("binding", {})
            print(f"WeClawBot 已绑定：{binding.get('device_id', 'device')}（凭据：{credentials_path()}）")
            return 0
        if args.weclawbot_action == "doctor":
            result = doctor_status(args.online)
            if args.json:
                print(json.dumps(result, ensure_ascii=False, indent=2))
            else:
                for check in result["checks"]:
                    mark = "OK" if check["ok"] else "FAIL"
                    print(f"{mark} {check['name']}: {check['detail']}")
            return 0 if result["ok"] else 1
        if args.weclawbot_action == "export":
            text = export_profile(args.format, args.include_secret)
            if args.output:
                output = Path(args.output).expanduser()
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_text(text, encoding="utf-8")
                os.chmod(output, 0o600)
                print(f"WeClawBot MQTT 配置已导出：{output}")
            else:
                print(text, end="")
            return 0
        if args.weclawbot_action == "unbind":
            if not args.yes:
                print("请加 --yes 确认删除本地 WeClawBot MQTT 凭据。")
                return 2
            path = forget_binding()
            print(f"WeClawBot 本地绑定已删除：{path}")
            return 0
        if args.weclawbot_action == "thinking":
            publish_activity("thinking", args.id, args.ttl)
            print(f"WeClawBot 已进入思考状态：{args.id}")
            return 0
        if args.weclawbot_action == "idle":
            publish_activity("idle", args.id)
            print(f"WeClawBot 已回到闲置状态：{args.id}")
            return 0
        if args.weclawbot_action == "screen":
            with open(args.document, "r", encoding="utf-8") as source:
                document = json.load(source)
            publish_screen_document(document)
            print(f"WeClawBot 已更新屏幕：{document.get('id', 'document')}")
            return 0
        if args.weclawbot_action == "clear":
            publish_screen_clear(args.target)
            print("WeClawBot 已发送清屏命令")
            return 0
        print("Usage: hermes weclawbot {bind|status|doctor|export|unbind|thinking|idle|screen|clear}")
        return 2
    except WeClawBotError as error:
        print(f"WeClawBot：{error}")
        return 1


def _status_payload(payload: dict | None) -> dict:
    if not payload:
        return {"paired": False, "credentials_path": str(credentials_path())}
    mqtt = payload.get("mqtt") if isinstance(payload.get("mqtt"), dict) else {}
    masked = dict(mqtt)
    if masked.get("password"):
        masked["password"] = "********"
    return {
        "paired": True,
        "credentials_path": str(credentials_path()),
        "binding": payload.get("binding", {}),
        "mqtt": masked,
    }
