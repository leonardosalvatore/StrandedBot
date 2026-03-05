import json
import os
import threading
from typing import Any

import ollama


WIDTH = 600
HEIGHT = 600
TITLE = "Bots"

TILE_SIZE = 40
GRID_WIDTH = WIDTH // TILE_SIZE
GRID_HEIGHT = HEIGHT // TILE_SIZE

PLAYER_RADIUS = 12
PLAYER_SPEED = 220

TILE_TYPES = {"grass", "sand", "water", "forest", "home", "road"}
TILE_COLORS = {
    "grass": (80, 170, 80),
    "sand": (220, 200, 120),
    "water": (70, 130, 220),
    "forest": (30, 110, 30),
    "home": (190, 120, 90),
    "road": (120, 120, 120),
}

player_x = WIDTH // 2
player_y = HEIGHT // 2

tiles: dict[tuple[int, int], str] = {}
tiles_lock = threading.Lock()

OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "llama3.2")
OLLAMA_ENABLED = os.getenv("OLLAMA_SCENERY", "1") == "1"


def _initialize_default_tiles() -> None:
    with tiles_lock:
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                tiles[(x, y)] = "grass"


def CreateTile(x: int, y: int, type: str) -> dict[str, Any]:
    if type not in TILE_TYPES:
        return {"ok": False, "error": f"Unknown tile type: {type}"}
    if not (0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT):
        return {"ok": False, "error": "Tile out of bounds"}

    with tiles_lock:
        tiles[(x, y)] = type

    return {"ok": True, "x": x, "y": y, "type": type}


def _tool_schema() -> list[dict[str, Any]]:
    return [
        {
            "type": "function",
            "function": {
                "name": "CreateTile",
                "description": "Create or update a tile at grid position (x, y).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "x": {
                            "type": "integer",
                            "description": f"Tile x coordinate from 0 to {GRID_WIDTH - 1}",
                        },
                        "y": {
                            "type": "integer",
                            "description": f"Tile y coordinate from 0 to {GRID_HEIGHT - 1}",
                        },
                        "type": {
                            "type": "string",
                            "enum": ["grass", "sand", "water", "forest", "home", "road"],
                        },
                    },
                    "required": ["x", "y", "type"],
                },
            },
        }
    ]


def _build_scenery_with_ollama() -> None:
    if not OLLAMA_ENABLED:
        return

    messages: list[dict[str, Any]] = [
        {
            "role": "system",
            "content": (
                "You are a terrain generator for a 15x15 tile map. "
                "Use only the CreateTile tool to build the map. "
                "Allowed types are grass, sand, water, forest, home, road. "
                "Create roads and at least one home."
            ),
        },
        {
            "role": "user",
            "content": (
                "Generate a coherent map for coordinates x=0..14 and y=0..14. "
                "Prefer natural-looking regions and a road path."
            ),
        },
    ]

    for _ in range(20):
        try:
            response = ollama.chat(model=OLLAMA_MODEL, messages=messages, tools=_tool_schema())
        except Exception:
            return

        message = response.get("message", {})
        tool_calls = message.get("tool_calls", [])

        messages.append(
            {
                "role": "assistant",
                "content": message.get("content", ""),
                "tool_calls": tool_calls,
            }
        )

        if not tool_calls:
            return

        for call in tool_calls:
            function_data = call.get("function", {})
            name = function_data.get("name")
            args = function_data.get("arguments", {})

            result: dict[str, Any]
            if name != "CreateTile":
                result = {"ok": False, "error": f"Unknown tool: {name}"}
            else:
                x = int(args.get("x", -1))
                y = int(args.get("y", -1))
                tile_type = str(args.get("type", ""))
                result = CreateTile(x=x, y=y, type=tile_type)

            messages.append(
                {
                    "role": "tool",
                    "name": name or "CreateTile",
                    "content": json.dumps(result),
                }
            )


def _start_scenery_generation() -> None:
    worker = threading.Thread(target=_build_scenery_with_ollama, daemon=True)
    worker.start()


def update(dt: float) -> None:
    global player_x, player_y

    dx = 0
    dy = 0

    if keyboard.w:
        dy -= 1
    if keyboard.s:
        dy += 1
    if keyboard.a:
        dx -= 1
    if keyboard.d:
        dx += 1

    player_x += dx * PLAYER_SPEED * dt
    player_y += dy * PLAYER_SPEED * dt

    player_x = max(PLAYER_RADIUS, min(WIDTH - PLAYER_RADIUS, player_x))
    player_y = max(PLAYER_RADIUS, min(HEIGHT - PLAYER_RADIUS, player_y))


def draw() -> None:
    screen.clear()

    with tiles_lock:
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                tile_type = tiles.get((x, y), "grass")
                color = TILE_COLORS[tile_type]
                screen.draw.filled_rect(
                    Rect((x * TILE_SIZE, y * TILE_SIZE), (TILE_SIZE, TILE_SIZE)), color
                )

    screen.draw.filled_circle((player_x, player_y), PLAYER_RADIUS, (0, 120, 255))


def run() -> None:
    import pgzrun

    pgzrun.go()


_initialize_default_tiles()
_start_scenery_generation()
