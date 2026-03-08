import json
import os
import subprocess
import threading
import time
from typing import Any

import ollama


def get_ollama_settings() -> tuple[str, bool]:
    model = os.getenv("OLLAMA_MODEL", "ministral-3:8b")
    enabled = os.getenv("OLLAMA_PLAY", "0") == "1"
    return model, enabled


def build_ollama_tools(lookfar_distance: int) -> list[dict[str, Any]]:
    return [
        {
            "type": "function",
            "function": {
                "name": "MoveTo",
                "description": (
                    "Move the bot toward a target tile. Specify absolute tile coordinates (x, y). "
                    "The bot will move up to 10 tiles per call, respecting terrain limits and water barriers. "
                    "Terrain limits: gravel/sand/habitat/crate=5 tiles, rocks=1 tile, water=blocked. "
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
                "name": "RepairHabitat",
                "description": (
                    "Repair the habitat on the bot's current tile. "
                    "The bot must be standing on a 'habitat' tile. "
                    "Repairing takes 2 consecutive calls to RepairHabitat and costs 10 energy total (5 per step). "
                    "Each habitat has a damage value (0-100%). Your mission is to repair all habitats. "
                    "LookClose and LookFar show habitat damage levels."
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
    habitats_repaired = sum(1 for habitat in game_logic.habitat_damage.values() if habitat["repaired"])
    return (
        "You are a robot explorer on Mars in a 2D tile-based survival game. "
        f"The map is {game_logic.GRID_WIDTH}x{game_logic.GRID_HEIGHT} tiles. "
        "All coordinates are TILE coordinates (x,y in grid), not pixel coordinates. "
        f"You run on battery. Your starting energy is {game_logic.bot_start_energy}. "
        "Every action (MoveTo, LookClose, LookFar, OpenCrate, TakeAllFromCrate, RepairHabitat, Thinking) costs energy. Moving costs 1 energy per tile. RepairHabitat costs 5 energy per step. "
        "If your energy reaches 0 you shut down — game over! "
        f"Every {game_logic.STEPS_SOLAR_FLARE_EVERY} steps there's a solar flare, you need to be in a habitat or you will be destroyed."
        f"\n\n🎯 MISSION: Repair all damaged habitats on Mars! Progress: {habitats_repaired}/{habitats_total} habitats repaired."
        "\n\nAvailable tools:\n"
        "- LookClose: look around (3x3 tile grid). Shows habitat damage levels. Use this to see immediate surroundings.\n"
        f"- LookFar: wide scan (radius {game_logic.bot_lookfar_distance}). Returns notable features (rocks, habitat, crate) \n"
        "with coordinates, distance, and habitat damage levels. Use this to plan your route!\n"
        "Rocks and habitats block line of sight, you cannot see crate behind them, you need to go around."
        "Habitats save you from the solar flare.\n"
        "- MoveTo(target_x, target_y): move toward absolute target tile. Specify (x, y) grid coordinates. "
        "Moves up to 10 tiles per call, respecting terrain limits and water barriers. You can move in diagonal too not just straight lines. "
        "Terrain speed limits: gravel/sand/habitat/crate=5 tiles/call, rocks=1 tile/call, water=blocked. "
        "Terrain limit is based on your STARTING tile.\n"
        "- OpenCrate: open a crate on your current tile to see how much energy is inside.\n"
        "- TakeAllFromCrate: take all energy from an opened crate (adds to your battery).\n"
        "- RepairHabitat: repair the habitat you're standing on. Takes 2 consecutive calls (10 energy total). Call RepairHabitat twice in a row while on the same habitat tile.\n"
        "\nTile types: gravel, sand, water, rocks, habitat, crate. "
        "Water is dangerous — avoid it. "
        "Rocks and habitats may hide things behind them. "
        "RED CRATES contain energy cells (0-100 energy) — find and loot them to survive! "
        "HABITATS need repair — each has damage 0-100%. Your goal is to repair ALL habitats."
        "\n\nStrategy: Balance energy collection and habitat repair. Use LookFar to find habitats and crates, "
        "MoveTo(x, y) to navigate efficiently on gravel or sand, "
        "single tiles in rock fields. Avoid water! "
        "Collect energy from crates (OpenCrate + TakeAllFromCrate) when battery is low. "
        "Move to habitats and call RepairHabitat twice in a row to fully repair them. "
        "Return to habitats before solar flares! "
        "Always explain your reasoning briefly before calling a tool. "
        "Keep working until all habitats are repaired!"
    )


def run_ollama_play_loop(game_logic: Any, model: str) -> None:
    print(f"\n{'='*60}")
    print(f"OLLAMA PLAY MODE — model: {model}")
    print(f"{'='*60}\n")

    messages: list[dict[str, Any]] = [
        {"role": "user", "content": build_base_prompt(game_logic)},
    ]
    tools = build_ollama_tools(game_logic.bot_lookfar_distance)
    tool_dispatch = game_logic.get_tool_dispatch()

    step = 0
    while True:
        step += 1

        while True:
            if game_logic.bot_state not in ("Moving", "Charging"):
                break
            time.sleep(0.1)

        print(f"\n--- Step {step} ---")

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

        messages.append(msg)

        tool_calls = msg.get("tool_calls") or []
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
        game_logic.print_step_status()

        if game_logic.bot_energy <= 0:
            print("\n  *** ROBOT SHUT DOWN — OUT OF ENERGY ***")
            break

        time.sleep(0.5)


def start_ollama_play(game_logic: Any, model: str) -> threading.Thread:
    worker = threading.Thread(target=run_ollama_play_loop, args=(game_logic, model), daemon=True)
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
