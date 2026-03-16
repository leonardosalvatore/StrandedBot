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
last_bot_speech = ""
OLLAMA_LINUX_DOCS_URL = "https://docs.ollama.com/linux"


    # Some models ignore native tool-calling and instead print a JSON snippet
    # like {"name": "MoveTo", "arguments": {...}} in plain text. We scan
    # fenced JSON blocks (and then full content) to recover those intents.
def _coerce_tool_calls(msg: dict[str, Any]) -> list[dict[str, Any]]:
    """Return structured tool calls from metadata or JSON content fallback."""
    tool_calls = msg.get("tool_calls") or []
    if tool_calls:
        return tool_calls

    content = (msg.get("content") or "").strip()
    if not content:
        return []

    candidates: list[str] = []
    fence_matches = re.findall(r"```(?:json)?\s*(\{.*?\}|\[.*?\])\s*```", content, flags=re.DOTALL)
    candidates.extend(fence_matches)
    candidates.append(content)

    parsed: Any = None
    for candidate in candidates:
        try:
            parsed = json.loads(candidate)
            break
        except json.JSONDecodeError:
            continue

    if parsed is None:
        return []

    items = parsed if isinstance(parsed, list) else [parsed]
    normalized: list[dict[str, Any]] = []
    for item in items:
        if not isinstance(item, dict):
            continue

        if "function" in item and isinstance(item["function"], dict):
            fn_name = item["function"].get("name", "")
            fn_args = item["function"].get("arguments", {})
        else:
            fn_name = item.get("name", "")
            fn_args = item.get("arguments", {})

        if not isinstance(fn_name, str) or not fn_name.strip():
            continue

        normalized.append(
            {
                "function": {
                    "name": fn_name.strip(),
                    "arguments": fn_args if isinstance(fn_args, (dict, str)) else {},
                }
            }
        )

    return normalized


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


def build_ollama_tools(lookfar_distance: int, rocks_required: int) -> list[dict[str, Any]]:
    return [
        {
            "type": "function",
            "function": {
                "name": "MoveTo",
                "description": (
                    "Move the bot toward a target tile. Specify absolute tile coordinates (x, y). "
                    "The bot can move over any tile type. "
                    "The bot will move up to 20 tiles per call. "
                    "Costs 1 energy per tile of movement."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "target_x": {
                            "type": "integer",
                            "description": "Target tile X coordinate (0 to 99).",
                        },
                        "target_y": {
                            "type": "integer",
                            "description": "Target tile Y coordinate (0 to 79).",
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
                    "returns a list of notable features (rocks, habitat, crate) with "
                    "their absolute tile coordinates (x, y), type, and distance. "
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
                "name": "OpenCrate",
                "description": (
                    "Open the crate on the bot's current tile. "
                    "The crate must be a 'crate' tile. Reveals how much energy is inside. "
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
                "name": "TakeAllFromCrate",
                "description": (
                    "Take all energy from the opened crate on the bot's current tile. "
                    "The crate must have been opened first with OpenCrate. "
                    "Adds the crate's energy to the bot's energy. "
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
                "name": "Dig",
                "description": (
                    "Dig a rock from a 'rocks' tile on your current location. "
                    "Replaces the rocks tile with gravel and adds 1 rock to your inventory. "
                    f"Collect {rocks_required} rocks to build 1 new habitat module. "
                    "This can help you expand safe shelter and improve your survival chances. "
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
                "name": "CreateHabitat",
                "description": (
                    "Create a new habitat on your current tile. "
                    f"Requires at least {rocks_required} rocks in inventory. "
                    f"Consumes exactly {rocks_required} rocks when successful. "
                    "Cannot be used on water or crate tiles. "
                    "Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            },
        },
    ]


def build_base_prompt(game_logic: Any) -> str:
    habitats_total = len(game_logic.habitat_damage)
    rocks_required = game_logic.ROCKS_REQUIRED_FOR_HABITAT
    return (
        "You are a robot explorer. Don't ask any questions it will cost you energy.\n"
        f"The map is {game_logic.GRID_WIDTH}x{game_logic.GRID_HEIGHT} tiles. "
        f"You run on battery. Solar flares occur every {game_logic.HOURS_SOLAR_FLARE_EVERY} hours and DESTROY ALL HABITATS while draining 100 energy unless you are in a habitat at flare time. "
        f"Build habitats by digging {rocks_required} rocks. "
        f"- Dig: dig a rock from a rocks tile on your current location. Adds 1 rock to inventory. {rocks_required} rocks = 1 new buildable habitat.\n"
        f"- CreateHabitat: build a habitat on your current tile using {rocks_required} rocks from inventory.\n"
        "WARNING: Solar flares DESTROY all habitats and drain 100 energy if you are not sheltered! Keep your energy high by collecting crates. "
        "Your MISSION is to survive by collecting crates for energy and managing resources wisely."
        f"Here are the tools you can use to complete your MISSION.\n"
        "- MoveTo: move toward a target tile, up to 20 tiles per call, over any tile type.\n"
        "- LookClose: look at the tiles adjacent to your current location.\n"
        "- LookFar: look at the tiles within your vision range.\n"
        "- OpenCrate: open a crate on your current tile to see how much energy is inside.\n"
        "- TakeAllFromCrate: take all energy from an opened crate (adds to your battery).\n"
        )


def run_ollama_play_loop(game_logic: Any, model: str, initial_prompt: str | None = None, interactive_mode: bool = False) -> None:
    global waiting_for_user_reply, last_bot_speech
    print(f"\n{'='*60}")
    print(f"OLLAMA PLAY MODE — model: {model}")
    print(f"Interactive mode: {interactive_mode}")
    print(f"{'='*60}\n")

    prompt_text = initial_prompt.strip() if isinstance(initial_prompt, str) and initial_prompt.strip() else build_base_prompt(game_logic)
    messages: list[dict[str, Any]] = [
        {"role": "user", "content": prompt_text},
    ]
    tools = build_ollama_tools(game_logic.bot_lookfar_distance, game_logic.ROCKS_REQUIRED_FOR_HABITAT)
    tool_dispatch = game_logic.get_tool_dispatch()

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

        try:
            response = ollama.chat(
                model=model,
                messages=messages,
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
                print("  [System] Detected question - waiting for user reply...")
                # Wait for user reply
                user_reply = user_reply_queue.get()
                print(f"  [User Reply] {user_reply}")
                messages.append(msg)
                messages.append({
                    "role": "user",
                    "content": user_reply,
                })
                waiting_for_user_reply = False
                continue

        messages.append(msg)

        tool_calls = _coerce_tool_calls(msg)
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
