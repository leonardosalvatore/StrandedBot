import json
import os
import queue
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


def _coerce_tool_calls(msg: dict[str, Any]) -> list[dict[str, Any]]:
    """Return only native tool calls from the API response."""
    return msg.get("tool_calls") or []


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
    cable_rocks_required: int,
    solar_panel_rocks_required: int,
) -> list[dict[str, Any]]:
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
                    "returns a list of notable features (rocks, habitat, cable, solar_panel) with "
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
                "name": "Dig",
                "description": (
                    "Dig a rock from a 'rocks' tile on your current location. "
                    "Replaces the rocks tile with gravel and adds 1 rock to your inventory. "
                    f"Build costs: habitat={habitat_rocks_required}, cable={cable_rocks_required}, solar_panel={solar_panel_rocks_required}. "
                    "Use it to gather materials for settlement construction. "
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
                "name": "Create",
                "description": (
                    "Create a structure on the current tile. "
                    f"Build costs: habitat={habitat_rocks_required} rocks, "
                    f"cable={cable_rocks_required} rock, solar_panel={solar_panel_rocks_required} rocks. "
                    "Cannot be used on water tiles. "
                    "Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "tile_type": {
                            "type": "string",
                            "enum": ["habitat", "cable", "solar_panel"],
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
    cable_rocks_required = game_logic.ROCKS_REQUIRED_FOR_CABLE
    solar_panel_rocks_required = game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL
    habitat_solar_charge = game_logic.HABITAT_SOLAR_CHARGE
    return (
        "You are a robot explorer.\n"
        "Goal: survive solar flares, then build an organized settlement.\n"
        f"Map size: {game_logic.GRID_WIDTH}x{game_logic.GRID_HEIGHT}.\n"
        f"Solar flare every {game_logic.HOURS_SOLAR_FLARE_EVERY} hours get in a habitat before the flare hits to survive.\n"
        f"If your current habitat is connected to a solar panel through cables, you gain +{habitat_solar_charge} energy each hour.\n"
        f"Build cost: habitat={habitat_rocks_required}, cable={cable_rocks_required}, solar_panel={solar_panel_rocks_required} rock(s).\n"
        "Keep your energy always more the 100.\n"
        "Build strategy:\n"
        "1) Build a tiny habitat first (1 habitat tile) to survive early solar flares.\n"
        "2) Power it immediately with cable + solar_panel so it becomes a safe charging shelter.\n"
        "3) After survival is stable, build solar fields (clusters of solar_panel tiles).\n"
        "4) Build groups of habitat tiles in a nice shape (compact block or symmetric pattern).\n"
        "5) Connect each habitat group to a solar field using a straight line of 10 cable tiles when possible.\n"
        "6) Expand settlement while preserving clean, intentional layout.\n"
        "Tool-calling rules:\n"
        "- Use the API tool calling interface for actions.\n"
        "- Do not write tool calls in normal text.\n"
        "- Do not emit JSON blobs or patterns like ToolName[ARGS]{...}. If native tool calling fails, say so plainly instead of faking a call.\n"
        "- Only describe inventory, energy and map changes after the corresponding tool result confirms them.\n"
        "- Never claim habitats, cables, solar panels, or charging unless STATUS or a tool result confirms them.\n"
        "Available tools: MoveTo, LookClose, LookFar, Dig, Create(tile_type)."
        )


def run_ollama_play_loop(game_logic: Any, model: str, initial_prompt: str | None = None, interactive_mode: bool = False) -> None:
    global waiting_for_user_reply, last_bot_speech
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
                "Do not claim a state change until a tool result confirms it."
            ),
        },
        {"role": "user", "content": prompt_text},
    ]
    tools = build_ollama_tools(
        game_logic.bot_lookfar_distance,
        game_logic.ROCKS_REQUIRED_FOR_HABITAT,
        game_logic.ROCKS_REQUIRED_FOR_CABLE,
        game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL,
    )
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

        gx, gy = game_logic._bot_grid_pos()
        current_tile = game_logic.tile_matrix[gx][gy].type
        rocks_count = sum(
            1 for item in (getattr(game_logic, "bot_inventory", []) or [])
            if isinstance(item, dict) and item.get("type") == "rock"
        )
        habitats_total = len(getattr(game_logic, "habitat_damage", {}))
        charging_possible = (
            current_tile == "habitat"
            and game_logic._is_habitat_connected_to_solar(gx, gy)
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

            # Fallback for old prompts/models that still call the legacy tool.
            if fn_name == "CreateHabitat":
                fn_name = "Create"
                if not isinstance(fn_args, dict):
                    fn_args = {}
                fn_args.setdefault("tile_type", "habitat")

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
