import json
import os
import queue
import re
import subprocess
import threading
import time
from typing import Any

import ollama

from bots import game_logic

# Interactive mode state
user_reply_queue: queue.Queue = queue.Queue()
waiting_for_user_reply = False
_close_user_reply_dialog_requested = False
USER_REPLY_TIMEOUT_SEC = 10.0
last_bot_speech = ""
OLLAMA_LINUX_DOCS_URL = "https://docs.ollama.com/linux"


def consume_user_reply_dialog_close_request() -> bool:
    """True once after a reply timeout so the UI thread can close the dialog."""
    global _close_user_reply_dialog_requested
    if not _close_user_reply_dialog_requested:
        return False
    _close_user_reply_dialog_requested = False
    return True


def _normalize_text_tool_call_payload(
    payload: Any,
    allowed_tool_names: set[str],
) -> list[dict[str, Any]]:
    """Normalize common JSON tool-call payload shapes into Ollama-style tool calls."""
    normalized: list[dict[str, Any]] = []

    def _append_call(name: str, arguments: Any) -> None:
        if not isinstance(name, str) or not name.strip():
            return
        tool_name = name.strip()
        if tool_name not in allowed_tool_names:
            return
        if isinstance(arguments, str):
            try:
                arguments = json.loads(arguments)
            except json.JSONDecodeError:
                arguments = {}
        if not isinstance(arguments, dict):
            arguments = {}
        normalized.append(
            {
                "function": {
                    "name": tool_name,
                    "arguments": arguments,
                }
            }
        )

    if isinstance(payload, list):
        for item in payload:
            normalized.extend(_normalize_text_tool_call_payload(item, allowed_tool_names))
        return normalized

    if not isinstance(payload, dict):
        return normalized

    if isinstance(payload.get("tool_calls"), list):
        for tc in payload["tool_calls"]:
            normalized.extend(_normalize_text_tool_call_payload(tc, allowed_tool_names))
        return normalized

    fn = payload.get("function")
    if isinstance(fn, dict):
        _append_call(fn.get("name", ""), fn.get("arguments", {}))
        return normalized

    if "name" in payload:
        _append_call(payload.get("name", ""), payload.get("arguments", payload.get("args", {})))
        return normalized

    if "tool" in payload:
        _append_call(payload.get("tool", ""), payload.get("arguments", payload.get("args", {})))

    return normalized


def _coerce_tool_calls(msg: dict[str, Any], allowed_tool_names: set[str]) -> list[dict[str, Any]]:
    """Return native tool calls or parse JSON tool calls embedded in assistant text."""
    native_calls = msg.get("tool_calls") or []
    if native_calls:
        return native_calls

    content = (msg.get("content") or "").strip()
    if not content:
        return []

    json_candidates: list[str] = []

    # Prefer fenced JSON blocks first because many models emit tool calls this way.
    fence_pattern = re.compile(r"```(?:json)?\s*(.*?)\s*```", re.IGNORECASE | re.DOTALL)
    for match in fence_pattern.findall(content):
        candidate = match.strip()
        if candidate:
            json_candidates.append(candidate)

    # If no fenced block exists, try the full message body as JSON.
    if not json_candidates and (content.startswith("{") or content.startswith("[")):
        json_candidates.append(content)

    normalized_calls: list[dict[str, Any]] = []
    for candidate in json_candidates:
        try:
            payload = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        normalized_calls.extend(_normalize_text_tool_call_payload(payload, allowed_tool_names))

    return normalized_calls


def get_ollama_settings() -> tuple[str, bool]:
    model = os.getenv("OLLAMA_MODEL", game_logic.OLLAMA_MODEL)
    #model = os.getenv("OLLAMA_MODEL", "lfm2.5-thinking:1.2b")
    enabled = os.getenv("OLLAMA_PLAY", "0") == "1"
    return model, enabled


def submit_user_reply(reply: str) -> None:
    """Submit a user reply to the bot."""
    global waiting_for_user_reply
    user_reply_queue.put(reply)
    waiting_for_user_reply = False


def is_waiting_for_reply() -> bool:
    """Check if bot is waiting for user reply."""
    return waiting_for_user_reply


def get_last_bot_speech() -> str:
    """Get the last bot speech for display."""
    return last_bot_speech


def is_ollama_running() -> bool:
    """Return True when the local Ollama server is reachable."""
    try:
        ollama.list()
        return True
    except Exception:
        return False


def build_ollama_tools(
    lookfar_distance: int,
    habitat_rocks_required: int,
    battery_rocks_required: int,
    solar_panel_rocks_required: int,
) -> list[dict[str, Any]]:
    max_tx = game_logic.GRID_WIDTH - 1
    max_ty = game_logic.GRID_HEIGHT - 1
    return [
        {
            "type": "function",
            "function": {
                "name": "MoveTo",
                "description": (
                    "Move the bot toward a target tile. Specify absolute tile indices (x, y) on the "
                    f"world grid ({game_logic.GRID_WIDTH} tiles wide × {game_logic.GRID_HEIGHT} tiles tall). "
                    "The bot can move over any tile type. "
                    "The bot will move up to 20 tiles per call. "
                    "Costs 1 energy per tile of movement."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "target_x": {
                            "type": "integer",
                            "minimum": 0,
                            "maximum": max_tx,
                            "description": f"Target tile X index from 0 to {max_tx} (inclusive).",
                        },
                        "target_y": {
                            "type": "integer",
                            "minimum": 0,
                            "maximum": max_ty,
                            "description": f"Target tile Y index from 0 to {max_ty} (inclusive).",
                        },
                    },
                    "required": ["target_x", "target_y"],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "LookClose",
                "description": (
                    "Look around: returns the 3x3 grid of tiles surrounding the bot, "
                    "including coordinates, tile type and description for each. "
                    "Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "LookFar",
                "description": (
                    f"Wide-area scan: looks in a radius of {lookfar_distance} tiles around the bot and "
                    "returns a list of notable features (rocks, habitat, battery, solar_panel) with "
                    f"their absolute tile coordinates (x, y) on the same grid as MoveTo "
                    f"(0–{game_logic.GRID_WIDTH - 1} by 0–{game_logic.GRID_HEIGHT - 1}), type, and distance. "
                    "Rock fields block line-of-sight, so features hidden behind rocks won't be visible. "
                    "Great for planning where to go next. Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "Dig",
                "description": (
                    "Dig a rock from a 'rocks' tile on your current location. "
                    "Each tool call removes at most one rock: the tile becomes gravel, so you cannot Dig again on the same tile—"
                    "MoveTo another 'rocks' tile to dig more. "
                    "To gather N rocks, issue N separate Dig calls (from valid rocks tiles), not one call with a count. "
                    "Adds 1 rock to inventory per successful Dig. "
                    f"Build costs: habitat={habitat_rocks_required}, battery={battery_rocks_required}, solar_panel={solar_panel_rocks_required}. "
                    "Costs 1 energy per Dig call."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "Create",
                "description": (
                    "Create a structure on the current tile. You cannot build on habitat, battery, or solar_panel tiles. "
                    f"Build costs: habitat={habitat_rocks_required} rocks, "
                    f"battery={battery_rocks_required} rock, solar_panel={solar_panel_rocks_required} rocks. "
                    "Cannot be used on water tiles. "
                    "Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "tile_type": {
                            "type": "string",
                            "enum": ["habitat", "battery", "solar_panel"],
                            "description": "Type of structure to create on the current tile.",
                        }
                    },
                    "required": ["tile_type"],
                },
            },
        },
    ]


def build_base_prompt(game_logic: Any) -> str:
    habitat_rocks_required = game_logic.ROCKS_REQUIRED_FOR_HABITAT
    battery_rocks_required = game_logic.ROCKS_REQUIRED_FOR_BATTERY
    solar_panel_rocks_required = game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL
    habitat_solar_charge = game_logic.HABITAT_SOLAR_CHARGE
    return ("YOUR MISSION IS:\n"
            "Build a 4x4 square of habitats and 3 batteries. Then attach to the square a line of 4 solar panels. \n"
            "Then lookfar and move 20 tiles and build a much bigger habitat. \n"
            "Never go back to check if they are powered, just keep going in random direction and build bigger new settlements. \n"
            "If you need many rocks: the Dig tool adds one rock per call—issue multiple Dig tool calls, moving between rocks tiles as needed "
            "(after each Dig that tile is gravel).\n").strip()
    # return (
    #     "You are a robot on a mission to prepare the biggest settlement for humans.\n"
    #     f"Map size: {game_logic.GRID_WIDTH}x{game_logic.GRID_HEIGHT}.\n"
    #     f"Solar flare every {game_logic.HOURS_SOLAR_FLARE_EVERY} hours get in a habitat before the flare hits to survive.\n"
    #     f"If your current habitat is connected to a solar panel and a battery, you gain +{habitat_solar_charge} energy each hour.\n"
    #     #f"Build cost: habitat={habitat_rocks_required}, battery={battery_rocks_required}, solar_panel={solar_panel_rocks_required} rock(s).\n"
    #     "Keep your energy always more the 200.\n"
    #     "YOUR MISSION:\n"
    #     "1) Grab rocks to gather materials for construction, 10 rocks to start. \n"
    #     "2) Build Habitat and power it immediately with battery + solar_panel so it becomes a safe charging shelter. Use this to keep your energy above 200\n"
    #     "3) After survival is stable, build a town with group of solar panels , group habitat and batteries, resembling a small settlement.\n"
    #     "Tool-calling rules:\n"
    #     "- Use the API tool calling interface for actions.\n"
    #     "- Do not write tool calls in normal text.\n"
    #     "- Do not emit JSON blobs or patterns like ToolName[ARGS]{...}. If native tool calling fails, say so plainly instead of faking a call.\n"
    #     "- Only describe inventory, energy and map changes after the corresponding tool result confirms them.\n"
    #     "- Never claim habitats, batteries, solar panels, or charging unless STATUS or a tool result confirms them.\n"
    #     "Available tools: MoveTo, LookClose, LookFar, Dig, Create(tile_type)."
    #     )


def run_ollama_play_loop(game_logic: Any, model: str, initial_prompt: str | None = None, interactive_mode: bool = False) -> None:
    global waiting_for_user_reply, last_bot_speech, _close_user_reply_dialog_requested
    print(f"\n{'='*60}")
    print(f"OLLAMA PLAY MODE — model: {model}")
    print(f"Interactive mode: {interactive_mode}")
    print(f"{'='*60}\n")

    prompt_text = initial_prompt.strip() if isinstance(initial_prompt, str) and initial_prompt.strip() else build_base_prompt(game_logic)
    messages: list[dict[str, Any]] = [
        {
            "role": "system",
            "content": (
                "Use native tool calls with the provided tools. "
                "Do not print tool invocations as text, JSON, or ToolName[ARGS]{...} unless native tool calling fails. "
                "Do not claim a state change until a tool result confirms it. "
                "Dig removes exactly one rock per tool call; gather several rocks with several Dig calls (and MoveTo other rocks tiles after each Dig)."
            ),
        },
        {"role": "user", "content": prompt_text},
    ]
    tools = build_ollama_tools(
        game_logic.bot_lookfar_distance,
        game_logic.ROCKS_REQUIRED_FOR_HABITAT,
        game_logic.ROCKS_REQUIRED_FOR_BATTERY,
        game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL,
    )
    tool_dispatch = game_logic.get_tool_dispatch()
    allowed_tool_names = set(tool_dispatch.keys())

    hour = 0
    while True:
        hour += 1
        game_logic.bot_hour_count = hour
        if not game_logic._advance_solar_flare_hour(hour):
            print("  [System] Bot destroyed by solar flare. Stopping play loop.")
            return

        while True:
            if game_logic.bot_state not in ("Moving", "Charging"):
                break
            time.sleep(0.1)

        print(f"\n--- hour {hour} ---")

        game_logic.bot_state = "Thinking"
        game_logic._consume_energy(1)

        gx, gy = game_logic._bot_grid_pos()
        current_tile_obj = game_logic.tile_matrix[gx][gy]
        current_tile = current_tile_obj.type
        rocks_count = sum(
            1 for item in (getattr(game_logic, "bot_inventory", []) or [])
            if isinstance(item, dict) and item.get("type") == "rock"
        )
        habitats_total = len(getattr(game_logic, "habitat_damage", {}))
        charging_possible = (
            current_tile == "habitat"
            and bool(getattr(current_tile_obj, "powered", False))
        )
        status_grounding = (
            "SYSTEM STATUS (truth source): "
            f"hour={hour}, energy={game_logic.bot_energy}, position=({gx},{gy}), "
            f"tile={current_tile}, rocks={rocks_count}, habitats={habitats_total}, "
            f"hours_to_solar_flare={game_logic.HOURS_TO_SOLAR_FLARE}, "
            f"charging_possible={charging_possible}. "
            "Use these exact values. If you are unsure, ask to LookClose or LookFar rather than inventing state."
        )

        try:
            response = ollama.chat(
                model=model,
                messages=messages + [{"role": "user", "content": status_grounding}],
                tools=tools,
            )
        except Exception as exc:
            print(f"  [Ollama] Error: {exc}")
            time.sleep(5)
            continue

        if isinstance(response, dict):
            msg = response.get("message", {})
        else:
            msg = getattr(response, "message", {})
            if not isinstance(msg, dict):
                msg = {
                    "role": getattr(msg, "role", "assistant"),
                    "content": getattr(msg, "content", ""),
                    "tool_calls": getattr(msg, "tool_calls", None),
                }

        content = msg.get("content", "") or ""
        if content.strip():
            print(f"  [🤖] {content.strip()}")
            game_logic.bot_last_speech = content.strip()
            last_bot_speech = content.strip()
            
            # Check for question in interactive mode
            if interactive_mode and "?" in content:
                waiting_for_user_reply = True
                print(
                    f"  [System] Detected question - waiting for user reply "
                    f"({int(USER_REPLY_TIMEOUT_SEC)}s timeout, then default \"Yes\")..."
                )
                try:
                    user_reply = user_reply_queue.get(timeout=USER_REPLY_TIMEOUT_SEC)
                except queue.Empty:
                    user_reply = "Yes"
                    print('  [System] No reply within timeout; defaulting to "Yes".')
                    _close_user_reply_dialog_requested = True
                else:
                    print(f"  [User Reply] {user_reply}")
                waiting_for_user_reply = False
                messages.append(msg)
                messages.append({
                    "role": "user",
                    "content": user_reply,
                })
                continue

        messages.append(msg)

        tool_calls = _coerce_tool_calls(msg, allowed_tool_names)
        if tool_calls and not (msg.get("tool_calls") or []):
            print(f"  [System] Parsed {len(tool_calls)} JSON tool call(s) from assistant text.")
        if not tool_calls and game_logic.bot_x == game_logic.bot_target_x and game_logic.bot_y == game_logic.bot_target_y:
            game_logic.bot_state = "Waiting"
            print("  [System] No tool call — nudging bot to continue exploring.")
            messages.append(
                {
                    "role": "user",
                    "content": "Keep exploring! Use LookClose to look around or MoveTo to go somewhere.",
                }
            )
            time.sleep(1)
            continue

        for tc in tool_calls:
            if isinstance(tc, dict):
                fn_name = tc.get("function", {}).get("name", "")
                fn_args = tc.get("function", {}).get("arguments", {})
            else:
                fn_obj = getattr(tc, "function", None)
                fn_name = getattr(fn_obj, "name", "") if fn_obj else ""
                fn_args = getattr(fn_obj, "arguments", {}) if fn_obj else {}

            if isinstance(fn_args, str):
                try:
                    fn_args = json.loads(fn_args)
                except json.JSONDecodeError:
                    fn_args = {}

            print(f"  [Tool Call] {fn_name}({fn_args})")

            game_logic.bot_state = game_logic.TOOL_STATE.get(fn_name, "Waiting")

            func = tool_dispatch.get(fn_name)
            if func is None:
                result = {"ok": False, "error": f"Unknown tool: {fn_name}"}
            else:
                try:
                    result = func(**fn_args)
                except Exception as exc:
                    result = {"ok": False, "error": str(exc)}

            messages.append(
                {
                    "role": "tool",
                    "content": json.dumps(result),
                }
            )

        game_logic.bot_state = "Waiting"
        game_logic.print_hour_status()

        if game_logic.bot_energy <= 0:
            print("\n  *** ROBOT SHUT DOWN — OUT OF ENERGY ***")
            break

        time.sleep(0.5)


def start_ollama_play(
    game_logic: Any,
    model: str,
    initial_prompt: str | None = None,
    interactive_mode: bool = False,
) -> threading.Thread | None:
    if not is_ollama_running():
        print("[Ollama] Ollama does not appear to be running.")
        print(f"[Ollama] Start/install instructions: {OLLAMA_LINUX_DOCS_URL}")
        return None

    worker = threading.Thread(
        target=run_ollama_play_loop,
        args=(game_logic, model, initial_prompt, interactive_mode),
        daemon=True,
    )
    worker.start()
    return worker


def stop_ollama_model(model: str) -> None:
    try:
        subprocess.run(
            ["ollama", "stop", model],
            timeout=5,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        print(f"  [Ollama] Stopped model {model}")
    except Exception:
        pass
