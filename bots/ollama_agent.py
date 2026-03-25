import json
import os
import queue
import re
import subprocess
import threading
import time
from collections.abc import Callable
from typing import Any

import ollama

from bots import game_logic

# Interactive mode state
user_reply_queue: queue.Queue = queue.Queue()
waiting_for_user_reply = False
_close_user_reply_dialog_requested = False
USER_REPLY_TIMEOUT_SEC = 10.0
_user_reply_deadline_lock = threading.Lock()
_user_reply_deadline_monotonic: float | None = None
last_bot_speech = ""
OLLAMA_LINUX_DOCS_URL = "https://docs.ollama.com/linux"


def _sim_hour_wall_interval_sec() -> float:
    """Wall seconds per in-game hour while blocked on the LLM. 0 = disabled (old behavior)."""
    raw = os.getenv("BOTS_SIM_HOUR_WALL_SEC", "2").strip()
    if not raw:
        return 0.0
    try:
        return max(0.0, float(raw))
    except ValueError:
        return 2.0


def _llm_context_game_hours() -> int:
    """Max game-hour span of chat history sent to Ollama (excluding fixed prefix). <=0 = unlimited."""
    raw = os.getenv("BOTS_LLM_CONTEXT_GAME_HOURS", "100").strip()
    if not raw:
        return 100
    try:
        return int(raw)
    except ValueError:
        return 100


def _trim_messages_for_game_hour_window(
    messages: list[dict[str, Any]],
    message_hours: list[int],
    current_hour: int,
    window_hours: int,
    *,
    prefix_len: int = 2,
) -> tuple[list[dict[str, Any]], list[int]]:
    """
    Keep system + initial user (first prefix_len), then only messages from game hours
    in [current_hour - window_hours + 1, current_hour]. Ensures we do not start the
    suffix with orphan tool messages (extends backward to include the owning assistant).
    """
    n = len(messages)
    if window_hours <= 0 or n <= prefix_len or n != len(message_hours):
        return messages, message_hours
    min_hour = current_hour - window_hours + 1
    kept: set[int] = set(range(prefix_len))
    for i in range(prefix_len, n):
        if message_hours[i] >= min_hour:
            kept.add(i)
    if len(kept) == prefix_len:
        return messages[:prefix_len], message_hours[:prefix_len]
    sorted_kept = sorted(kept)
    first_suffix = next(k for k in sorted_kept if k >= prefix_len)
    if messages[first_suffix].get("role") == "tool":
        low = first_suffix - 1
        while low >= prefix_len and messages[low].get("role") == "tool":
            low -= 1
        if low >= prefix_len:
            kept.update(range(low, first_suffix))
        sorted_kept = sorted(kept)
    trimmed_msgs = [messages[i] for i in sorted_kept]
    trimmed_hrs = [message_hours[i] for i in sorted_kept]
    return trimmed_msgs, trimmed_hrs


def _wait_model_chat_with_sim_hours(
    game_logic: Any,
    chat_fn: Callable[[], Any],
    wall_sec_per_game_hour: float,
) -> tuple[Any | None, bool]:
    """
    Run chat_fn in a daemon thread. While waiting, advance bot_hour_count and solar-flare
    logic every wall_sec_per_game_hour so time does not freeze during slow models.

    Returns (response, destroyed). If destroyed, response may be None and the play loop should stop.
    """
    result: list[Any] = []
    error: list[BaseException] = []
    done = threading.Event()

    def _target() -> None:
        try:
            result.append(chat_fn())
        except BaseException as exc:
            error.append(exc)
        finally:
            done.set()

    threading.Thread(target=_target, daemon=True).start()

    if wall_sec_per_game_hour <= 0:
        done.wait()
        if error:
            raise error[0]
        return result[0], False

    next_tick = time.monotonic() + wall_sec_per_game_hour
    while not done.is_set():
        now = time.monotonic()
        if now >= next_tick:
            game_logic.bot_hour_count += 1
            if not game_logic._advance_solar_flare_hour(game_logic.bot_hour_count):
                print(
                    "  [System] Bot destroyed by solar flare while waiting for model (sim time advanced)."
                )
                return (result[0] if result else None), True
            print(
                f"  [SimTime] hour {game_logic.bot_hour_count} elapsed while waiting for model"
            )
            next_tick += wall_sec_per_game_hour
            continue
        done.wait(timeout=min(0.05, max(0.001, next_tick - now)))

    if error:
        raise error[0]
    return result[0], False


def _start_user_reply_deadline() -> None:
    global _user_reply_deadline_monotonic
    with _user_reply_deadline_lock:
        _user_reply_deadline_monotonic = time.monotonic() + USER_REPLY_TIMEOUT_SEC


def _clear_user_reply_deadline() -> None:
    global _user_reply_deadline_monotonic
    with _user_reply_deadline_lock:
        _user_reply_deadline_monotonic = None


def bump_user_reply_deadline() -> None:
    """UI thread: full idle period restarts when the user types in the reply box."""
    global _user_reply_deadline_monotonic
    if not waiting_for_user_reply:
        return
    with _user_reply_deadline_lock:
        if _user_reply_deadline_monotonic is None:
            return
        _user_reply_deadline_monotonic = time.monotonic() + USER_REPLY_TIMEOUT_SEC


def get_user_reply_seconds_remaining() -> float | None:
    """Seconds until auto-default reply, or None if not waiting."""
    if not waiting_for_user_reply:
        return None
    with _user_reply_deadline_lock:
        if _user_reply_deadline_monotonic is None:
            return None
        return max(0.0, _user_reply_deadline_monotonic - time.monotonic())


def _wait_for_user_reply_interactive() -> tuple[str, bool]:
    """Return (reply, timed_out). timed_out True means default \"Yes\" from idle expiry."""
    global _close_user_reply_dialog_requested
    _start_user_reply_deadline()
    poll = 0.05
    while True:
        with _user_reply_deadline_lock:
            deadline = _user_reply_deadline_monotonic
            remaining = (
                max(0.0, deadline - time.monotonic()) if deadline is not None else 0.0
            )
        if remaining <= 0.0:
            print('  [System] No reply within timeout; defaulting to "Yes".')
            _close_user_reply_dialog_requested = True
            _clear_user_reply_deadline()
            return "Yes", True
        try:
            msg = user_reply_queue.get(timeout=min(poll, max(0.01, remaining)))
        except queue.Empty:
            continue
        _clear_user_reply_deadline()
        text = msg if isinstance(msg, str) else str(msg)
        return text, False


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
    _clear_user_reply_deadline()


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
                    "returns a list of notable features (rocks, habitat, battery, solar_panel, turret) with "
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
                    f"Build costs: habitat={habitat_rocks_required}, battery={battery_rocks_required}, "
                    f"solar_panel={solar_panel_rocks_required}, turret={game_logic.ROCKS_REQUIRED_FOR_TURRET}. "
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
                    "Create a structure on the current tile. You cannot build on existing structures. "
                    f"Build costs: habitat={habitat_rocks_required} rocks, "
                    f"battery={battery_rocks_required} rock, solar_panel={solar_panel_rocks_required} rocks, "
                    f"turret={game_logic.ROCKS_REQUIRED_FOR_TURRET} rock. "
                    "Cannot be used on water or broken_habitat. "
                    "Turret: defense tile; fires an instant white laser at nearby ants at a real-time rate (see game settings) "
                    "if orthogonally adjacent to a solar_panel and a habitat or battery. Ants take several hits to destroy. "
                    "Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "tile_type": {
                            "type": "string",
                            "enum": ["habitat", "battery", "solar_panel", "turret"],
                            "description": "Type of structure to create on the current tile.",
                        }
                    },
                    "required": ["tile_type"],
                },
            },
        },
        {
            "type": "function",
            "function": {
                "name": "ListBuiltTiles",
                "description": (
                    "Returns every structure the bot successfully placed via Create: list of "
                    "{x, y, type, game_hour} on the same grid as MoveTo. "
                    "Append-only log—tiles may have changed since (solar flares, etc.); "
                    "use LookClose or LookFar to verify the live map. Costs 1 energy."
                ),
                "parameters": {
                    "type": "object",
                    "properties": {},
                    "required": [],
                },
            },
        },
    ]


def build_base_prompt(game_logic: Any, scenario: str = "Explorer") -> str:
    habitat_rocks_required = game_logic.ROCKS_REQUIRED_FOR_HABITAT
    battery_rocks_required = game_logic.ROCKS_REQUIRED_FOR_BATTERY
    solar_panel_rocks_required = game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL
    habitat_solar_charge = game_logic.HABITAT_SOLAR_CHARGE
    scenario_norm = str(scenario).strip()

    common_tail = (
        "When solar flare is coming, move to a habitat or battery and wait for it to pass or be sure to have 200 energy.\n"
    )

    if scenario_norm == "Tower Defense":
        return (
            "YOUR MISSION:\n"
            "Defend your starting tiny town. Stay near the base and reinforce it by building habitats, batteries, solar panels, and turrets. "
            "Build where ants are approaching and where solar flare events threaten your habitats.\n"
            "Never go back to verify solar wiring; keep reacting and rebuilding forward.\n"
            + common_tail
        ).strip()

    if scenario_norm == "Builder":
        return (
            "YOUR MISSION:\n"
            "Expand habitats to build a big town. Create habitats, batteries, and solar panels in expanding clusters.\n"
            "Never go back to verify solar wiring; keep pushing expansion forward.\n"
            + common_tail
        ).strip()

    # Explorer (default)
    return (
        "YOUR MISSION:\n"
        "Explore the map. Build habitat and the required solar+storage network only to recharge. "
        "Keep moving forward instead of looping.\n"
        "Never go back to verify solar wiring; keep exploring and building new settlements.\n"
        + common_tail
    ).strip()



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
    # Game hour when each message was appended (for rolling context). Matches `messages` length.
    message_hours: list[int] = [0, 0]
    context_hours = _llm_context_game_hours()
    if context_hours > 0:
        print(
            f"  [LLM] Chat context: last {context_hours} game hours "
            f"(system + initial mission always kept). Set BOTS_LLM_CONTEXT_GAME_HOURS=0 for unlimited."
        )
    tools = build_ollama_tools(
        game_logic.bot_lookfar_distance,
        game_logic.ROCKS_REQUIRED_FOR_HABITAT,
        game_logic.ROCKS_REQUIRED_FOR_BATTERY,
        game_logic.ROCKS_REQUIRED_FOR_SOLAR_PANEL,
    )
    tool_dispatch = game_logic.get_tool_dispatch()
    allowed_tool_names = set(tool_dispatch.keys())

    sim_wall = _sim_hour_wall_interval_sec()
    if sim_wall > 0:
        print(
            f"  [SimTime] While the model responds, 1 game hour passes every {sim_wall}s wall time "
            f"(set BOTS_SIM_HOUR_WALL_SEC=0 to disable).\n"
        )

    while True:
        game_logic.bot_hour_count += 1
        hour = game_logic.bot_hour_count
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
        habitat_hourly_charge_active = game_logic.bot_habitat_network_charge_active()
        status_grounding = (
            "SYSTEM STATUS (truth source): "
            f"hour={game_logic.bot_hour_count}, energy={game_logic.bot_energy}, position=({gx},{gy}), "
            f"tile={current_tile}, rocks={rocks_count}, habitats={habitats_total}, "
            f"hours_to_solar_flare={game_logic.HOURS_TO_SOLAR_FLARE}, "
            f"habitat_hourly_charge_active={habitat_hourly_charge_active}. "
            "Use these exact values. If you are unsure, ask to LookClose or LookFar rather than inventing state."
        )

        def _append_message(m: dict[str, Any]) -> None:
            messages.append(m)
            message_hours.append(hour)

        def _prune_local_context() -> None:
            if context_hours <= 0:
                return
            nm, nh = _trim_messages_for_game_hour_window(
                messages,
                message_hours,
                game_logic.bot_hour_count,
                context_hours,
            )
            messages.clear()
            message_hours.clear()
            messages.extend(nm)
            message_hours.extend(nh)

        base_msgs, _ = _trim_messages_for_game_hour_window(
            messages,
            message_hours,
            game_logic.bot_hour_count,
            context_hours,
        )
        msgs_for_chat = base_msgs + [{"role": "user", "content": status_grounding}]

        try:
            response, destroyed_waiting = _wait_model_chat_with_sim_hours(
                game_logic,
                lambda: ollama.chat(
                    model=model,
                    messages=msgs_for_chat,
                    tools=tools,
                ),
                sim_wall,
            )
        except Exception as exc:
            print(f"  [Ollama] Error: {exc}")
            time.sleep(5)
            continue

        if destroyed_waiting:
            print("  [System] Stopping play loop (simulation ended during model wait).")
            return

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
                    f"({int(USER_REPLY_TIMEOUT_SEC)}s idle, then default \"Yes\"; typing resets the timer)..."
                )
                user_reply, reply_timed_out = _wait_for_user_reply_interactive()
                if not reply_timed_out:
                    print(f"  [User Reply] {user_reply}")
                waiting_for_user_reply = False
                _append_message(msg)
                _append_message(
                    {
                        "role": "user",
                        "content": user_reply,
                    }
                )
                _prune_local_context()
                continue

        _append_message(msg)

        tool_calls = _coerce_tool_calls(msg, allowed_tool_names)
        if tool_calls and not (msg.get("tool_calls") or []):
            print(f"  [System] Parsed {len(tool_calls)} JSON tool call(s) from assistant text.")
        if not tool_calls and game_logic.bot_x == game_logic.bot_target_x and game_logic.bot_y == game_logic.bot_target_y:
            game_logic.bot_state = "Waiting"
            print("  [System] No tool call — nudging bot to continue exploring.")
            _append_message(
                {
                    "role": "user",
                    "content": "Keep exploring! Use LookClose to look around or MoveTo to go somewhere.",
                }
            )
            _prune_local_context()
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

            _append_message(
                {
                    "role": "tool",
                    "content": json.dumps(result),
                }
            )

        _prune_local_context()

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
