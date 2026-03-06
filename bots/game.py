import atexit
import importlib.resources
import json
import os
import random
import subprocess
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import ollama
import pygame


MAP_WIDTH = 1000
SIDEBAR_WIDTH = 200
WIDTH = MAP_WIDTH + SIDEBAR_WIDTH
MAP_HEIGHT = 800
PANEL_HEIGHT = 400
HEIGHT = MAP_HEIGHT + PANEL_HEIGHT
TITLE = "Bots"

TILE_SIZE = 10
GRID_WIDTH = MAP_WIDTH // TILE_SIZE
GRID_HEIGHT = MAP_HEIGHT // TILE_SIZE

# Camera/viewport settings
VIEWPORT_TILES_W = 50  # Show 50x50 tile area
VIEWPORT_TILES_H = 50
DRAW_TILE_SIZE = MAP_WIDTH // VIEWPORT_TILES_W  # 50px per tile when rendered

BOT_RADIUS = 10
BOT_SPEED = 220

TILE_TYPES = {"grass", "sand", "water", "forest", "home", "road", "crate"}
TILE_COLORS = {
    "grass": (80, 170, 80),
    "sand": (220, 200, 120),
    "water": (70, 130, 220),
    "forest": (30, 110, 30),
    "home": (190, 120, 90),
    "road": (120, 120, 120),
    "crate": (200, 50, 50),
}

TILE_DESCRIPTIONS = {
    "grass": "A flat patch of green grass.",
    "sand": "Warm, loose sand.",
    "water": "Clear, shimmering water.",
    "forest": "Dense trees and undergrowth.",
    "home": "A small dwelling.",
    "road": "A well-trodden dirt path.",
    "crate": "A mysterious red crate. It might contain energy cells.",
}

# Maximum move distance allowed FROM each tile type
TILE_MAX_DISTANCE: dict[str, int] = {
    "grass": 5,
    "sand": 5,
    "water": 0,
    "forest": 1,
    "home": 5,
    "road": 10,
    "crate": 5,
}


@dataclass
class Tile:
    x: int
    y: int
    type: str
    color: tuple[int, int, int] = field(default=(80, 170, 80))
    description: str = field(default="A flat patch of green grass.")


bot_x = 300
bot_y = 300
bot_target_x: float = bot_x
bot_target_y: float = bot_y
bot_energy = 100
bot_inventory: list[dict[str, Any]] = []
bot_state: str = "Waiting"  # Waiting | Thinking | Moving | LookClose | LookFar | Charging
bot_last_speech: str = ""  # Last text the LLM said, shown at screen bottom

# --- Spritesheet setup (450x300, 6 sprites of 150x150) ---
_SPRITE_SHEET: pygame.Surface | None = None
_SPRITE_SIZE = 150
# Map bot_state -> (col, row) in the spritesheet
_STATE_SPRITE_POS: dict[str, tuple[int, int]] = {
    "Waiting":   (0, 0),
    "Thinking":  (1, 0),
    "Moving":    (2, 0),
    "LookClose": (0, 1),
    "LookFar":   (1, 1),
    "Charging":  (2, 1),
}

tiles: dict[tuple[int, int], str] = {}
# Crate contents: maps (x, y) -> {"energy": int, "opened": bool}
crate_contents: dict[tuple[int, int], dict[str, Any]] = {}
tile_matrix: list[list[Tile]] = [
    [Tile(x=x, y=y, type="grass") for y in range(GRID_HEIGHT)]
    for x in range(GRID_WIDTH)
]
tiles_lock = threading.Lock()

#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen2.5-coder:latest")
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen3.5:9b") # not moving much
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen3:0.6b") # not calling the tools
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "lfm2.5-thinking:1.2b") #fast but not always call the tools
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "ministral-3:3b") # fast and funny
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "ministral-3:8b") # fast and funny, more coherent than 3b
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "ministral-3:14b") # too big for my 12GB video card
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "granite4:1b") # too small
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen3-vl:4b")


OLLAMA_PLAY = os.getenv("OLLAMA_PLAY", "0") == "1"


# ---------------------------------------------------------------------------
# Procedural map generation (fast, no Ollama needed)
# ---------------------------------------------------------------------------

def _place_ellipse(cx: int, cy: int, rx: int, ry: int, tile_type: str, jitter: float = 0.3) -> int:
    """Place tiles in a rough elliptical shape. Returns count placed."""
    count = 0
    for x in range(max(0, cx - rx - 2), min(GRID_WIDTH, cx + rx + 3)):
        for y in range(max(0, cy - ry - 2), min(GRID_HEIGHT, cy + ry + 3)):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            dist = dx * dx + dy * dy
            # add some noise so edges aren't perfectly smooth
            noise = random.uniform(-jitter, jitter)
            if dist + noise < 1.0:
                CreateTile(x, y, tile_type)
                count += 1
    return count


def _place_border(cx: int, cy: int, rx: int, ry: int, tile_type: str, thickness: int = 2) -> int:
    """Place a border ring around an ellipse."""
    count = 0
    outer_rx, outer_ry = rx + thickness, ry + thickness
    for x in range(max(0, cx - outer_rx - 1), min(GRID_WIDTH, cx + outer_rx + 2)):
        for y in range(max(0, cy - outer_ry - 1), min(GRID_HEIGHT, cy + outer_ry + 2)):
            dx_o = (x - cx) / outer_rx
            dy_o = (y - cy) / outer_ry
            dx_i = (x - cx) / rx
            dy_i = (y - cy) / ry
            dist_outer = dx_o * dx_o + dy_o * dy_o
            dist_inner = dx_i * dx_i + dy_i * dy_i
            noise = random.uniform(-0.15, 0.15)
            if dist_outer + noise < 1.0 and dist_inner + noise >= 1.0:
                # don't overwrite the inner tiles
                with tiles_lock:
                    if tiles.get((x, y)) == "grass":
                        pass  # ok to place
                    else:
                        continue
                CreateTile(x, y, tile_type)
                count += 1
    return count


def _place_road_h(y: int, x0: int, x1: int) -> int:
    """Place a horizontal (east-west) road on row y from x0 to x1."""
    count = 0
    lo, hi = min(x0, x1), max(x0, x1)
    for x in range(lo, hi + 1):
        if 0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT:
            CreateTile(x, y, "road")
            count += 1
    return count


def _place_road_v(x: int, y0: int, y1: int) -> int:
    """Place a vertical (north-south) road on column x from y0 to y1."""
    count = 0
    lo, hi = min(y0, y1), max(y0, y1)
    for y in range(lo, hi + 1):
        if 0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT:
            CreateTile(x, y, "road")
            count += 1
    return count


def _place_road_l(x0: int, y0: int, x1: int, y1: int) -> int:
    """Place an L-shaped road: first horizontal then vertical."""
    count = _place_road_h(y0, x0, x1)
    count += _place_road_v(x1, y0, y1)
    return count


def _build_scenery_procedural() -> None:
    """Generate a maze-like map with forests, lakes, grid roads, village, and crates."""
    t0 = time.time()
    total = 0
    print("Generating map procedurally...")

    # --- Grid of axis-aligned roads (maze corridors) ---
    # Horizontal roads (east-west)
    h_roads = [10, 25, 40, 55, 70]
    for ry in h_roads:
        total += _place_road_h(ry, 2, GRID_WIDTH - 3)

    # Vertical roads (north-south)
    v_roads = [10, 30, 50, 70, 90]
    for rx in v_roads:
        total += _place_road_v(rx, 2, GRID_HEIGHT - 3)

    # Extra short connecting roads for variety
    total += _place_road_h(33, 10, 50)
    total += _place_road_h(48, 50, 90)
    total += _place_road_v(20, 10, 40)
    total += _place_road_v(60, 40, 70)
    total += _place_road_v(80, 10, 25)
    print(f"  Roads: done")

    # --- Forests filling maze cells (between roads) ---
    forests = [
        # (cx, cy, rx, ry)
        (20, 17, 7, 5),
        (40, 17, 6, 5),
        (60, 32, 7, 5),
        (80, 17, 6, 5),
        (20, 48, 7, 5),
        (80, 48, 7, 5),
        (40, 62, 7, 5),
        (80, 62, 7, 5),
        (15, 62, 5, 4),
        (60, 48, 5, 4),
    ]
    for cx, cy, rx, ry in forests:
        total += _place_ellipse(cx, cy, rx, ry, "forest", jitter=0.4)
    print(f"  Forests: {len(forests)} clusters done")

    # --- Lakes with sand beaches (in several maze cells) ---
    lakes = [
        # (cx, cy, rx, ry)
        (60, 17, 6, 5),
        (40, 33, 5, 4),
        (20, 33, 5, 4),
        (80, 33, 5, 4),
        (40, 48, 4, 3),
    ]
    for cx, cy, rx, ry in lakes:
        _place_border(cx, cy, rx, ry, "sand", thickness=2)
        total += _place_ellipse(cx, cy, rx, ry, "water", jitter=0.25)
    print(f"  Lakes: {len(lakes)} done")

    # --- Village (center area) ---
    village_homes = [
        (52, 57), (55, 57), (52, 60), (55, 60), (58, 63),
    ]
    for hx, hy in village_homes:
        CreateTile(hx, hy, "home")
        CreateTile(hx + 1, hy, "home")
        CreateTile(hx, hy + 1, "home")
        CreateTile(hx + 1, hy + 1, "home")
        total += 4
    for i in range(len(village_homes) - 1):
        hx0, hy0 = village_homes[i]
        hx1, hy1 = village_homes[i + 1]
        total += _place_road_l(hx0, hy0, hx1, hy1)
    print(f"  Village: done")

    # --- Random crates with energy ---
    crates_placed = 0
    attempts = 0
    while crates_placed < 15 and attempts < 500:
        attempts += 1
        cx = random.randint(2, GRID_WIDTH - 3)
        cy = random.randint(2, GRID_HEIGHT - 3)
        if tiles.get((cx, cy)) == "grass":
            CreateTile(cx, cy, "crate")
            crate_contents[(cx, cy)] = {
                "energy": random.randint(0, 100),
                "opened": False,
            }
            crates_placed += 1
            total += 1
    print(f"  Crates: placed {crates_placed} crates")

    elapsed = time.time() - t0
    print(f"Procedural map complete: {total} non-grass tiles in {elapsed:.3f}s")


def _initialize_default_tiles() -> None:
    with tiles_lock:
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                tiles[(x, y)] = "grass"
                tile_matrix[x][y] = Tile(
                    x=x, y=y, type="grass",
                    color=TILE_COLORS["grass"],
                    description=TILE_DESCRIPTIONS["grass"],
                )


def CreateTile(x: int, y: int, type: str) -> dict[str, Any]:
    if type not in TILE_TYPES:
        return {"ok": False, "error": f"Unknown tile type: {type}"}
    if not (0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT):
        return {"ok": False, "error": "Tile out of bounds"}

    color = TILE_COLORS[type]
    description = TILE_DESCRIPTIONS[type]

    with tiles_lock:
        tiles[(x, y)] = type
        tile_matrix[x][y] = Tile(
            x=x, y=y, type=type,
            color=color, description=description,
        )

    return {"ok": True, "x": x, "y": y, "type": type}


# ---------------------------------------------------------------------------
# LLM bot tools: Move, LookClose, LookFar, OpenCrate, TakeAllFromCrate
# ---------------------------------------------------------------------------

_DIRECTION_DELTAS = {
    "north": (0, -1),
    "south": (0, 1),
    "east": (1, 0),
    "west": (-1, 0),
    "northeast": (1, -1),
    "northwest": (-1, -1),
    "southeast": (1, 1),
    "southwest": (-1, 1),
}


def _consume_energy(amount: int = 1) -> None:
    """Decrease bot energy by amount."""
    global bot_energy
    bot_energy = max(0, bot_energy - amount)


def _bot_grid_pos() -> tuple[int, int]:
    """Return the bot's logical tile grid position (based on target)."""
    gx = max(0, min(GRID_WIDTH - 1, int(bot_target_x) // TILE_SIZE))
    gy = max(0, min(GRID_HEIGHT - 1, int(bot_target_y) // TILE_SIZE))
    return gx, gy


def Move(direction: str, distance: int = 1) -> dict[str, Any]:
    """Move the bot 1-10 tile steps in one of 8 directions, limited by terrain."""
    global bot_target_x, bot_target_y
    direction = direction.lower().strip()
    if direction not in _DIRECTION_DELTAS:
        msg = f"Unknown direction '{direction}'. Use: {', '.join(_DIRECTION_DELTAS)}"
        print(f"  [Move] ERROR: {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    distance = max(1, min(10, int(distance)))
    dx, dy = _DIRECTION_DELTAS[direction]

    # Check terrain speed limit at starting tile
    start_gx, start_gy = _bot_grid_pos()
    start_tile = tile_matrix[start_gx][start_gy]
    terrain_limit = TILE_MAX_DISTANCE.get(start_tile.type, 5)
    if terrain_limit == 0:
        msg = (f"Cannot move — stuck on {start_tile.type} at ({start_gx}, {start_gy})! "
               f"You should not be on water.")
        print(f"  [Move] BLOCKED: {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy,
                "tile_x": start_gx, "tile_y": start_gy, "tile_type": start_tile.type}

    actual_distance = min(distance, terrain_limit)

    # Step tile-by-tile, stopping before water
    steps_taken = 0
    new_x, new_y = bot_target_x, bot_target_y
    for _ in range(actual_distance):
        _consume_energy(1)
        next_x = new_x + dx * TILE_SIZE
        next_y = new_y + dy * TILE_SIZE
        next_x = max(BOT_RADIUS, min(MAP_WIDTH - BOT_RADIUS, next_x))
        next_y = max(BOT_RADIUS, min(MAP_HEIGHT - BOT_RADIUS, next_y))
        # Check what tile we'd land on
        next_gx = max(0, min(GRID_WIDTH - 1, int(next_x) // TILE_SIZE))
        next_gy = max(0, min(GRID_HEIGHT - 1, int(next_y) // TILE_SIZE))
        next_tile = tile_matrix[next_gx][next_gy]
        if next_tile.type == "water":
            print(f"  [Move] Stopped before water at ({next_gx}, {next_gy})")
            break
        new_x = next_x
        new_y = next_y
        steps_taken += 1

    bot_target_x = new_x
    bot_target_y = new_y

    grid_x, grid_y = _bot_grid_pos()
    landed = tile_matrix[grid_x][grid_y]

    if steps_taken < distance:
        reason = (f"terrain limit ({start_tile.type}={terrain_limit})"
                  if actual_distance < distance else "water ahead")
        print(f"  [Move] Moved {direction} {steps_taken}/{distance} tiles ({reason}) "
              f"→ ({grid_x}, {grid_y}) = {landed.type}")
    else:
        print(f"  [Move] Moved {direction} {steps_taken} tiles "
              f"→ ({grid_x}, {grid_y}) = {landed.type}")

    return {
        "ok": True,
        "direction": direction,
        "distance_requested": distance,
        "distance_moved": steps_taken,
        "terrain_limit": terrain_limit,
        "tile_x": grid_x,
        "tile_y": grid_y,
        "tile_type": landed.type,
        "tile_description": landed.description,
        "energy": bot_energy,
    }


def LookClose() -> dict[str, Any]:
    """Read the tiles surrounding the bot (3x3 grid centered on bot)."""
    _consume_energy(1)
    grid_x, grid_y = _bot_grid_pos()

    surrounding: list[dict[str, Any]] = []
    with tiles_lock:
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                nx, ny = grid_x + dx, grid_y + dy
                if 0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT:
                    t = tile_matrix[nx][ny]
                    label = "center" if dx == 0 and dy == 0 else ""
                    surrounding.append({
                        "x": t.x,
                        "y": t.y,
                        "type": t.type,
                        "description": t.description,
                        "position": label,
                    })

    print(f"  [LookClose] Bot at tile ({grid_x}, {grid_y}), "
          f"scanned {len(surrounding)} surrounding tiles")

    return {
        "ok": True,
        "bot_tile_x": grid_x,
        "bot_tile_y": grid_y,
        "surrounding": surrounding,
        "energy": bot_energy,
    }


_PANORAMA_TYPES = {"forest", "home", "road", "crate"}


def _tile_direction(dx: int, dy: int) -> str:
    """Return a compass direction string for a delta vector."""
    if dx == 0 and dy < 0:
        return "north"
    if dx == 0 and dy > 0:
        return "south"
    if dx > 0 and dy == 0:
        return "east"
    if dx < 0 and dy == 0:
        return "west"
    if dx > 0 and dy < 0:
        return "northeast"
    if dx < 0 and dy < 0:
        return "northwest"
    if dx > 0 and dy > 0:
        return "southeast"
    if dx < 0 and dy > 0:
        return "southwest"
    return "here"


def LookFar() -> dict[str, Any]:
    """Scan a wide area (radius 50) and return notable features with direction and distance."""
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    radius = 50

    features: list[dict[str, Any]] = []
    with tiles_lock:
        for dx in range(-radius, radius + 1):
            for dy in range(-radius, radius + 1):
                nx, ny = gx + dx, gy + dy
                if not (0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT):
                    continue
                t = tile_matrix[nx][ny]
                time.sleep(0.001)  # simulate processing time per tile
                if t.type in _PANORAMA_TYPES:
                    dist = max(abs(dx), abs(dy))  # Chebyshev distance
                    direction = _tile_direction(dx, dy)
                    features.append({
                        "direction": direction,
                        "type": t.type,
                        "distance": dist,
                        "x": nx,
                        "y": ny,
                    })

    # Deduplicate clusters: keep closest per (direction, type)
    best: dict[tuple[str, str], dict[str, Any]] = {}
    for f in features:
        key = (f["direction"], f["type"])
        if key not in best or f["distance"] < best[key]["distance"]:
            best[key] = f
    summary = sorted(best.values(), key=lambda f: f["distance"])

    print(f"  [LookFar] Scanned radius {radius} from ({gx}, {gy}): "
          f"found {len(summary)} notable features")
    for f in summary:
        print(f"    {f['type']:>7} {f['direction']:<10} dist={f['distance']}  at ({f['x']},{f['y']})")

    return {
        "ok": True,
        "bot_tile_x": gx,
        "bot_tile_y": gy,
        "features": summary,
        "energy": bot_energy,
    }


def OpenCrate() -> dict[str, Any]:
    """Open the crate on the bot's current tile (if any)."""
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    crate = crate_contents.get((gx, gy))

    if crate is None:
        msg = f"No crate at tile ({gx}, {gy})."
        print(f"  [OpenCrate] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    if crate["opened"]:
        msg = f"Crate at ({gx}, {gy}) is already open. Energy inside: {crate['energy']}."
        print(f"  [OpenCrate] {msg}")
        return {"ok": True, "already_opened": True, "energy_inside": crate["energy"],
                "energy": bot_energy}

    crate["opened"] = True
    print(f"  [OpenCrate] Opened crate at ({gx}, {gy}) — contains {crate['energy']} energy!")
    return {
        "ok": True,
        "already_opened": False,
        "energy_inside": crate["energy"],
        "tile_x": gx,
        "tile_y": gy,
        "energy": bot_energy,
    }


def TakeAllFromCrate() -> dict[str, Any]:
    """Take all energy from the opened crate on the bot's current tile."""
    global bot_energy
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    crate = crate_contents.get((gx, gy))

    if crate is None:
        msg = f"No crate at tile ({gx}, {gy})."
        print(f"  [TakeAllFromCrate] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    if not crate["opened"]:
        msg = f"Crate at ({gx}, {gy}) is not opened yet. Use OpenCrate first."
        print(f"  [TakeAllFromCrate] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    gained = crate["energy"]
    crate["energy"] = 0
    bot_energy += gained

    # Replace the crate tile with grass
    CreateTile(gx, gy, "grass")
    del crate_contents[(gx, gy)]

    print(f"  [TakeAllFromCrate] Took {gained} energy from crate at ({gx}, {gy}). "
          f"Bot energy now: {bot_energy}")
    time.sleep(gained/10)
    return {
        "ok": True,
        "energy_gained": gained,
        "energy": bot_energy,
        "tile_x": gx,
        "tile_y": gy,
    }


# Tool dispatch table
_TOOL_DISPATCH: dict[str, Any] = {
    "Move": Move,
    "LookClose": LookClose,
    "LookFar": LookFar,
    "OpenCrate": OpenCrate,
    "TakeAllFromCrate": TakeAllFromCrate,
}

# Map tool name -> bot_state to set before calling
_TOOL_STATE: dict[str, str] = {
    "Move": "Moving",
    "LookClose": "LookClose",
    "LookFar": "LookFar",
    "OpenCrate": "LookClose",
    "TakeAllFromCrate": "Charging",
}

# Ollama tool definitions for tool-calling API
_OLLAMA_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "Move",
            "description": (
                "Move the bot 1-10 tile steps in one of 8 compass directions. "
                "Actual distance is limited by terrain: road=10, grass/sand/home/crate=5, forest=1, water=0 (impassable). "
                "The limit is based on the tile you START on. Movement also stops before entering water. "
                "Costs 1 energy regardless of distance."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "direction": {
                        "type": "string",
                        "description": "The compass direction to move (e.g. 'north', 'southeast').",
                    },
                    "distance": {
                        "type": "integer",
                        "description": "Number of tiles to move (1-10). Clamped by terrain speed limit. Default is 1.",
                    },
                },
                "required": ["direction"],
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
                "Wide-area scan: looks in a radius of 50 tiles around the bot and "
                "returns a list of notable features (forest, home, road, crate) with "
                "their compass direction, type, and distance. "
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
]

_PLAY_BASE_PROMPT = (
    "You are a robot explorer in a 2D tile-based RPG world. "
    f"The map is {GRID_WIDTH}x{GRID_HEIGHT} tiles. "
    "All coordinates are TILE coordinates (x,y in grid), not pixel coordinates. "
    "You run on battery. Your starting energy is 1000. "
    "Every action (Move, LookClose, LookFar, OpenCrate, TakeAllFromCrate) costs 1 energy. "
    "If your energy reaches 0 you shut down \u2014 game over! "
    "\n\nAvailable tools:\n"
    "- LookClose: look around (3x3 tile grid). Use this to see immediate surroundings.\n"
    "- LookFar: wide scan (radius 50). Returns notable features (forest, home, road, crate) "
    "with direction and distance. Use this to plan your route!\n"
    "- Move(direction, distance): move 1-10 tiles in 8 directions (north/south/east/west/ne/nw/se/sw). "
    "Distance is limited by terrain: road=10, grass/sand/home/crate=5, forest=1, water=0 (impassable). "
    "The limit is based on the tile you START on. You also stop before entering water. "
    "Costs only 1 energy regardless of distance, so use roads for fast travel!\n"
    "- OpenCrate: open a crate on your current tile to see how much energy is inside.\n"
    "- TakeAllFromCrate: take all energy from an opened crate (adds to your battery).\n"
    "\nTile types: grass, sand, water, forest, home, road, crate. "
    "Water is dangerous — avoid it. "
    "Roads are safe. Homes are rest points. Forests may hide things. "
    "RED CRATES contain energy cells (0-100 energy) — find and loot them to survive! "
    "\n\nStrategy: use LookFar to find crates and notable features, "
    "Move(direction, distance=10) on roads for fast travel, distance=5 on grass, "
    "distance=1 in forests. Avoid water! LookClose when close, "
    "then OpenCrate + TakeAllFromCrate to loot energy. "
    "Explore efficiently to conserve battery. "
    "Always explain your reasoning briefly before calling a tool. "
    "Keep exploring — don't stop!"
)


def _print_step_status() -> None:
    """Print the bot's current surroundings, energy, and inventory."""
    gx, gy = _bot_grid_pos()
    print(f"\n  === STATUS ===")
    print(f"  Energy: {bot_energy}  |  Position: tile ({gx}, {gy})")
    print(f"  Inventory: {bot_inventory if bot_inventory else '(empty)'}")
    print(f"  Surroundings:")
    with tiles_lock:
        for dy in range(-1, 2):
            row = []
            for dx in range(-1, 2):
                nx, ny = gx + dx, gy + dy
                if 0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT:
                    t = tile_matrix[nx][ny]
                    marker = "*" if dx == 0 and dy == 0 else " "
                    row.append(f"{marker}{t.type:>7}({nx},{ny})")
                else:
                    row.append("     [OOB]    ")
            print(f"    {'  '.join(row)}")
    print(f"  ==============\n")


def _run_ollama_play_loop() -> None:
    """Run the LLM agent loop: the model calls Move/LookClose tools to explore the map."""
    global bot_state
    print(f"\n{'='*60}")
    print(f"OLLAMA PLAY MODE — model: {OLLAMA_MODEL}")
    print(f"{'='*60}\n")

    messages: list[dict[str, Any]] = [
        {"role": "user", "content": _PLAY_BASE_PROMPT},
    ]

    step = 0
    while True:
        step += 1

        # Wait for the bot to finish moving/charging before next LLM call
        while True:
            if bot_state not in ("Moving", "Charging"):
                break
            time.sleep(0.1)

        print(f"\n--- Step {step} ---")

        bot_state = "Thinking"
        try:
            response = ollama.chat(
                model=OLLAMA_MODEL,
                messages=messages,
                tools=_OLLAMA_TOOLS,
            )
        except Exception as e:
            print(f"  [Ollama] Error: {e}")
            time.sleep(5)
            continue

        # Extract the assistant message
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

        # Print any text the model said
        content = msg.get("content", "") or ""
        if content.strip():
            print(f"  [🤖] {content.strip()}")
            global bot_last_speech
            bot_last_speech = content.strip()

        messages.append(msg)

        # Check for tool calls
        tool_calls = msg.get("tool_calls") or []
        if not tool_calls and bot_x == bot_target_x and bot_y == bot_target_y:
            # No tool calls — nudge the model to keep going
            bot_state = "Waiting"
            print("  [System] No tool call — nudging bot to continue exploring.")
            messages.append({
                "role": "user",
                "content": "Keep exploring! Use LookClose to look around or Move to go somewhere.",
            })
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

            # Set bot_state based on tool being called
            bot_state = _TOOL_STATE.get(fn_name, "Waiting")

            func = _TOOL_DISPATCH.get(fn_name)
            if func is None:
                result = {"ok": False, "error": f"Unknown tool: {fn_name}"}
            else:
                try:
                    result = func(**fn_args)
                except Exception as e:
                    result = {"ok": False, "error": str(e)}

            # Feed the tool result back to the model
            messages.append({
                "role": "tool",
                "content": json.dumps(result),
            })

        # --- Print status after each step ---
        bot_state = "Waiting"
        _print_step_status()

        # Check for death
        if bot_energy <= 0:
            print("\n  *** ROBOT SHUT DOWN — OUT OF ENERGY ***")
            break

        # Small delay to avoid hammering Ollama
        time.sleep(0.5)


def _start_scenery_generation() -> None:
    _build_scenery_procedural()
    if OLLAMA_PLAY:
        worker = threading.Thread(target=_run_ollama_play_loop, daemon=True)
        worker.start()


def update(dt: float) -> None:
    global bot_x, bot_y, bot_target_x, bot_target_y, bot_state

    # --- Keyboard: move the target directly ---
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

    if dx or dy:
        bot_target_x += dx * BOT_SPEED * dt
        bot_target_y += dy * BOT_SPEED * dt
        bot_target_x = max(BOT_RADIUS, min(MAP_WIDTH - BOT_RADIUS, bot_target_x))
        bot_target_y = max(BOT_RADIUS, min(MAP_HEIGHT - BOT_RADIUS, bot_target_y))

    # --- Smoothly move visual position toward target ---
    move_speed = TILE_SIZE * 2  # 2 tiles per second
    diff_x = bot_target_x - bot_x
    diff_y = bot_target_y - bot_y
    dist = (diff_x ** 2 + diff_y ** 2) ** 0.5

    if dist > 0.5:
        bot_state = "Moving"
        step = move_speed * dt
        if step >= dist:
            bot_x = bot_target_x
            bot_y = bot_target_y
        else:
            bot_x += (diff_x / dist) * step
            bot_y += (diff_y / dist) * step
    elif bot_state == "Moving":
        bot_state = "Waiting"


def draw() -> None:
    global _SPRITE_SHEET
    screen.clear()

    # Camera position (centered on bot)
    gx, gy = _bot_grid_pos()
    cam_tile_x = gx
    cam_tile_y = gy

    # Calculate visible tile range (50x50 centered on bot)
    half_w = VIEWPORT_TILES_W // 2
    half_h = VIEWPORT_TILES_H // 2
    tile_x_start = max(0, cam_tile_x - half_w)
    tile_x_end = min(GRID_WIDTH, cam_tile_x + half_w + 1)
    tile_y_start = max(0, cam_tile_y - half_h)
    tile_y_end = min(GRID_HEIGHT, cam_tile_y + half_h + 1)

    # Camera center in world pixel coords
    cam_world_x = bot_x
    cam_world_y = bot_y

    with tiles_lock:
        for tx in range(tile_x_start, tile_x_end):
            for ty in range(tile_y_start, tile_y_end):
                tile_type = tiles.get((tx, ty), "grass")
                color = TILE_COLORS[tile_type]
                # World position of this tile
                world_x = tx * TILE_SIZE
                world_y = ty * TILE_SIZE
                # Screen position (camera-relative, scaled up)
                screen_x = (world_x - cam_world_x) * (DRAW_TILE_SIZE / TILE_SIZE) + MAP_WIDTH / 2
                screen_y = (world_y - cam_world_y) * (DRAW_TILE_SIZE / TILE_SIZE) + MAP_HEIGHT / 2
                screen.draw.filled_rect(
                    Rect((int(screen_x), int(screen_y)), (DRAW_TILE_SIZE, DRAW_TILE_SIZE)), color
                )

    # Load spritesheet on first draw (pygame is initialised by now)
    if _SPRITE_SHEET is None:
        try:
            _res = importlib.resources.files("bots.resources").joinpath("bots.png")
            with importlib.resources.as_file(_res) as _sheet_path:
                _SPRITE_SHEET = pygame.image.load(str(_sheet_path)).convert_alpha()
            print(f"  [Sprite] Loaded spritesheet from package resources")
        except Exception as e:
            print(f"  [Sprite] Failed to load bots.png: {e}")
            # Fallback: draw circle at screen center (camera follows bot)
            radius_scaled = int(BOT_RADIUS * (DRAW_TILE_SIZE / TILE_SIZE))
            screen.draw.filled_circle((MAP_WIDTH // 2, MAP_HEIGHT // 2), radius_scaled, (0, 120, 255))
            return

    # Pick the right sprite sub-rect based on bot_state
    col, row = _STATE_SPRITE_POS.get(bot_state, (0, 0))
    src_rect = pygame.Rect(
        col * _SPRITE_SIZE, row * _SPRITE_SIZE,
        _SPRITE_SIZE, _SPRITE_SIZE,
    )
    sprite = _SPRITE_SHEET.subsurface(src_rect)

    # Scale sprite to fit in the zoomed view
    # Bot is always at screen center when camera follows it
    draw_size = int(BOT_RADIUS * 6 * (DRAW_TILE_SIZE / TILE_SIZE))
    scaled = pygame.transform.smoothscale(sprite, (draw_size, draw_size))

    # Bot draws at screen center (camera follows bot)
    dest_x = int(MAP_WIDTH / 2) - draw_size // 2
    dest_y = int(MAP_HEIGHT / 2) - draw_size // 2
    screen.surface.blit(scaled, (dest_x, dest_y))

    # --- Right sidebar (stats) ---
    sidebar_x = MAP_WIDTH
    screen.draw.filled_rect(
        Rect((sidebar_x, 0), (SIDEBAR_WIDTH, MAP_HEIGHT)), (20, 20, 30)
    )
    screen.draw.line((sidebar_x, 0), (sidebar_x, MAP_HEIGHT), (60, 60, 80))

    pad = 10
    font_stats = pygame.font.SysFont("monospace", 16, bold=True)
    stat_lh = font_stats.get_linesize()
    sx = sidebar_x + pad
    sy = pad

    gx, gy = _bot_grid_pos()
    tgx = max(0, min(GRID_WIDTH - 1, int(bot_target_x) // TILE_SIZE))
    tgy = max(0, min(GRID_HEIGHT - 1, int(bot_target_y) // TILE_SIZE))

    stat_lines = [
        f"Energy: {bot_energy}",
        f"Pos: ({gx}, {gy})",
        f"Target: ({tgx}, {tgy})",
        f"State: {bot_state}",
        "",
        f"Model:",
        f" {OLLAMA_MODEL}",
    ]
    for i, line in enumerate(stat_lines):
        color = (180, 220, 255) if i < 4 else (140, 160, 200)
        surf = font_stats.render(line, True, color)
        screen.surface.blit(surf, (sx, sy + i * stat_lh))

    # --- Bottom info panel (speech) ---
    panel_y = MAP_HEIGHT
    screen.draw.filled_rect(
        Rect((0, panel_y), (WIDTH, PANEL_HEIGHT)), (20, 20, 30)
    )
    screen.draw.line((0, panel_y), (WIDTH, panel_y), (60, 60, 80))

    padding = 14
    font_speech = pygame.font.SysFont("monospace", 22)

    if bot_last_speech:
        speech_y = panel_y + padding
        line_height = font_speech.get_linesize()
        max_width = WIDTH - padding * 2 - 30  # margin for 🤖 prefix

        # Word-wrap into lines that fit the panel width
        words = bot_last_speech.split()
        lines: list[str] = []
        current = ""
        for word in words:
            test = f"{current} {word}".strip()
            if font_speech.size(test)[0] <= max_width:
                current = test
            else:
                if current:
                    lines.append(current)
                current = word
        if current:
            lines.append(current)

        # Cap lines to fit the remaining panel space
        remaining = PANEL_HEIGHT - padding * 2
        max_lines = max(1, remaining // line_height)
        if len(lines) > max_lines:
            lines = lines[:max_lines]
            lines[-1] = lines[-1][:-3] + "..." if len(lines[-1]) > 3 else "..."

        for i, line in enumerate(lines):
            prefix = "🤖 " if i == 0 else "   "
            text_surface = font_speech.render(f"{prefix}{line}", True, (220, 220, 220))
            screen.surface.blit(text_surface, (padding, speech_y + i * line_height))


def run() -> None:
    import pgzrun

    pgzrun.go()


def _stop_ollama_model() -> None:
    """Unload the Ollama model from memory on exit."""
    try:
        subprocess.run(["ollama", "stop", OLLAMA_MODEL], timeout=5,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"  [Ollama] Stopped model {OLLAMA_MODEL}")
    except Exception:
        pass  # best-effort

atexit.register(_stop_ollama_model)

_initialize_default_tiles()
_start_scenery_generation()
