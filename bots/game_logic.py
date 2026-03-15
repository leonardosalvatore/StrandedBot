import random
import threading
import time
import os
from dataclasses import dataclass, field
from typing import Any

MAP_WIDTH = 1400
SIDEBAR_WIDTH = 520
WIDTH = MAP_WIDTH + SIDEBAR_WIDTH
MAP_HEIGHT = 830
PANEL_HEIGHT = 500
HEIGHT = MAP_HEIGHT + PANEL_HEIGHT
TITLE = "Bots"

OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "ministral-3:8b")
OLLAMA_PLAY = os.getenv("OLLAMA_PLAY", "0") == "1"

TILE_SIZE = 4
GRID_WIDTH = MAP_WIDTH // TILE_SIZE
GRID_HEIGHT = MAP_HEIGHT // TILE_SIZE

VIEWPORT_TILES_W = 70
VIEWPORT_TILES_H = 70
DRAW_TILE_SIZE = MAP_WIDTH // VIEWPORT_TILES_W

BOT_RADIUS = 10
BOT_SPEED = 220
STEPS_SOLAR_FLARE_EVERY = 50
STEPS_TO_SOLAR_FLARE = STEPS_SOLAR_FLARE_EVERY
ROCKS_REQUIRED_FOR_HABITAT = 5

# Solar flare animation state
solar_flare_animation_active = False
solar_flare_animation_start_time = 0.0
solar_flare_last_step = -1

TILE_TYPES = {"gravel", "sand", "water", "rocks", "habitat", "crate"}
TILE_COLORS = {
    "gravel": (140, 120, 100),
    "sand": (220, 200, 120),
    "water": (120, 210, 255),
    "rocks": (80, 70, 60),
    "habitat": (180, 255, 100),
    "broken_habitat": (40, 70, 40),
    "crate": (200, 50, 50),
}

TILE_DESCRIPTIONS = {
    "gravel": "Loose red gravel and dust.",
    "sand": "Warm, loose sand.",
    "water": "Clear, shimmering water.",
    "rocks": "Jagged rocks and boulders.",
    "habitat": "A sealed habitat module.",
    "crate": "A mysterious red crate. It might contain energy cells.",
}

TILE_MAX_DISTANCE: dict[str, int] = {
    "gravel": 40,
    "sand": 30,
    "water": 6,
    "rocks": 20,
    "habitat": 10,
    "crate": 10,
}


@dataclass
class Tile:
    x: int
    y: int
    type: str
    color: tuple[int, int, int] = field(default=(80, 170, 80))
    description: str = field(default="Loose red gravel and dust.")
    fog: bool = field(default=True)


bot_start_energy = 200
bot_x = 400
bot_y = 550
bot_target_x: float = bot_x
bot_target_y: float = bot_y
bot_energy = bot_start_energy
bot_inventory: list[dict[str, Any]] = []
bot_state: str = "Waiting"
bot_last_speech: str = ""
bot_lookfar_distance = 40
bot_step_count: int = 0


tiles: dict[tuple[int, int], str] = {}
crate_contents: dict[tuple[int, int], dict[str, Any]] = {}
habitat_damage: dict[tuple[int, int], dict[str, Any]] = {}
tile_matrix: list[list[Tile]] = [
    [Tile(x=x, y=y, type="gravel") for y in range(GRID_HEIGHT)]
    for x in range(GRID_WIDTH)
]
tiles_lock = threading.Lock()

# Fog of war setting (False when OLLAMA_PLAY=0 for full visibility)
enable_fog_of_war = True


TOOL_STATE: dict[str, str] = {
    "MoveTo": "Moving",
    "LookClose": "LookClose",
    "LookFar": "LookFar",
    "OpenCrate": "LookClose",
    "TakeAllFromCrate": "Charging",
    "Dig": "LookClose",
    "CreateHabitat": "LookClose",
}


def _place_ellipse(cx: int, cy: int, rx: int, ry: int, tile_type: str, jitter: float = 0.3) -> int:
    count = 0
    for x in range(max(0, cx - rx - 2), min(GRID_WIDTH, cx + rx + 3)):
        for y in range(max(0, cy - ry - 2), min(GRID_HEIGHT, cy + ry + 3)):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            dist = dx * dx + dy * dy
            noise = random.uniform(-jitter, jitter)
            if dist + noise < 1.0:
                CreateTile(x, y, tile_type)
                count += 1
    return count


def _place_border(cx: int, cy: int, rx: int, ry: int, tile_type: str, thickness: int = 2) -> int:
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
                with tiles_lock:
                    if tiles.get((x, y)) != "gravel":
                        continue
                CreateTile(x, y, tile_type)
                count += 1
    return count


def _place_sand_patch(cx: int, cy: int, rx: int, ry: int) -> int:
    count = 0
    for x in range(max(0, cx - rx - 2), min(GRID_WIDTH, cx + rx + 3)):
        for y in range(max(0, cy - ry - 2), min(GRID_HEIGHT, cy + ry + 3)):
            dx = (x - cx) / max(1, rx)
            dy = (y - cy) / max(1, ry)
            dist = dx * dx + dy * dy
            noise = random.uniform(-0.25, 0.25)
            if dist + noise < 1.0:
                with tiles_lock:
                    if tiles.get((x, y)) != "gravel":
                        continue
                CreateTile(x, y, "sand")
                count += 1
    return count


def _carve_stream(start_x: int, start_y: int, length: int = 28, width: int = 1) -> int:
    count = 0
    x = start_x
    y = start_y
    dx = random.choice([-1, 1])
    dy = random.choice([-1, 0, 1])

    for _ in range(length):
        for wx in range(-width, width + 1):
            for wy in range(-width, width + 1):
                nx = x + wx
                ny = y + wy
                if not (0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT):
                    continue

                if CreateTile(nx, ny, "water")["ok"]:
                    count += 1

                # Add narrow sand banks around water to feel like a stream bed
                for sx in range(-2, 3):
                    for sy in range(-2, 3):
                        if max(abs(sx), abs(sy)) < 2:
                            continue
                        bx = nx + sx
                        by = ny + sy
                        if not (0 <= bx < GRID_WIDTH and 0 <= by < GRID_HEIGHT):
                            continue
                        with tiles_lock:
                            if tiles.get((bx, by)) != "gravel":
                                continue
                        CreateTile(bx, by, "sand")

        # Meandering stream movement with slight directional persistence
        if random.random() < 0.25:
            dx += random.choice([-1, 0, 1])
            dy += random.choice([-1, 0, 1])
            dx = max(-1, min(1, dx))
            dy = max(-1, min(1, dy))
            if dx == 0 and dy == 0:
                dx = random.choice([-1, 1])

        x += dx
        y += dy

        if x < 2 or x >= GRID_WIDTH - 2:
            dx *= -1
            x = max(2, min(GRID_WIDTH - 3, x))
        if y < 2 or y >= GRID_HEIGHT - 2:
            dy *= -1
            y = max(2, min(GRID_HEIGHT - 3, y))

    return count


def _place_rock_field(cx: int, cy: int, radius: int, density: float = 0.95) -> int:
    """Build an irregular rock blob using multiple random walkers."""
    target_tiles = max(24, int(radius * radius * 2.6 * density))
    max_attempts = target_tiles * 40
    walkers = [(cx, cy) for _ in range(random.randint(3, 6))]
    placed: set[tuple[int, int]] = set()
    count = 0
    attempts = 0

    while len(placed) < target_tiles and attempts < max_attempts:
        attempts += 1
        i = random.randrange(len(walkers))
        x, y = walkers[i]

        # Occasionally pull walkers back toward center to keep cohesive clusters.
        if random.random() < 0.18:
            if x < cx:
                x += 1
            elif x > cx:
                x -= 1
            if y < cy:
                y += 1
            elif y > cy:
                y -= 1

        x += random.choice([-1, 0, 1])
        y += random.choice([-1, 0, 1])

        if x < 1 or x >= GRID_WIDTH - 1 or y < 1 or y >= GRID_HEIGHT - 1:
            x = max(1, min(GRID_WIDTH - 2, x))
            y = max(1, min(GRID_HEIGHT - 2, y))

        dx = x - cx
        dy = y - cy
        if dx * dx + dy * dy > int((radius * 1.35) ** 2):
            x = cx + (dx // 2)
            y = cy + (dy // 2)

        walkers[i] = (x, y)

        if not (0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT):
            continue
        if (x - cx) * (x - cx) + (y - cy) * (y - cy) > int((radius * 1.45) ** 2):
            continue

        if CreateTile(x, y, "rocks")["ok"] and (x, y) not in placed:
            placed.add((x, y))
            count += 1

    return count


def _build_scenery_procedural() -> None:
    t0 = time.time()
    total = 0
    print("Generating map procedurally...")

    # Original terrain tuning was authored for 8px tiles.
    terrain_scale = 8 / TILE_SIZE

    def _scaled(v: int) -> int:
        return max(1, int(round(v * terrain_scale)))

    # Scale biome counts by map area relative to the original 8px-tile tuning.
    area_scale = terrain_scale * terrain_scale

    rock_fields = max(14, int(round(14 * area_scale)))
    rock_total = 0
    for _ in range(rock_fields):
        cx = random.randint(6, GRID_WIDTH - 7)
        cy = random.randint(6, GRID_HEIGHT - 7)
        radius = random.randint(_scaled(3), _scaled(8))
        rock_total += _place_rock_field(cx, cy, radius, density=random.uniform(0.85, 1.1))
    total += rock_total
    print(f"  Rocks: {rock_fields} irregular fields done")

    stream_count = max(6, int(round(6 * area_scale)))
    stream_total = 0
    for _ in range(stream_count):
        sx = random.randint(4, GRID_WIDTH - 5)
        sy = random.randint(4, GRID_HEIGHT - 5)
        stream_total += _carve_stream(
            sx,
            sy,
            length=random.randint(_scaled(26), _scaled(52)),
            width=random.choice([1, _scaled(1)]),
        )
    total += stream_total
    print(f"  Streams: {stream_count} carved")

    sand_patches = max(18, int(round(22 * area_scale)))
    sand_total = 0
    for _ in range(sand_patches):
        cx = random.randint(3, GRID_WIDTH - 4)
        cy = random.randint(3, GRID_HEIGHT - 4)
        rx = random.randint(_scaled(3), _scaled(7))
        ry = random.randint(_scaled(3), _scaled(7))
        sand_total += _place_sand_patch(cx, cy, rx, ry)
    total += sand_total
    print(f"  Sand patches: {sand_patches} clusters done")

    # habitat_sites = [
    #     (52, 57),
    #     (55, 57),
    #     (52, 60),
    #     (55, 60),
    #     (58, 63),
    # ]
    # for hx, hy in habitat_sites:
    #     CreateTile(hx, hy, "habitat")
    #     CreateTile(hx + 1, hy, "habitat")
    #     CreateTile(hx, hy + 1, "habitat")
    #     CreateTile(hx + 1, hy + 1, "habitat")
    #     # Initialize damage for each habitat tile
    #     habitat_damage[(hx, hy)] = {"damage": random.randint(0, 100), "repaired": False}
    #     habitat_damage[(hx + 1, hy)] = {"damage": random.randint(0, 100), "repaired": False}
    #     habitat_damage[(hx, hy + 1)] = {"damage": random.randint(0, 100), "repaired": False}
    #     habitat_damage[(hx + 1, hy + 1)] = {"damage": random.randint(0, 100), "repaired": False}
    #     # Set color based on damage status (broken habitats are dark green)
    #     tile_matrix[hx][hy].color = TILE_COLORS["broken_habitat"]
    #     tile_matrix[hx + 1][hy].color = TILE_COLORS["broken_habitat"]
    #     tile_matrix[hx][hy + 1].color = TILE_COLORS["broken_habitat"]
    #     tile_matrix[hx + 1][hy + 1].color = TILE_COLORS["broken_habitat"]
    #     total += 4
    # print("  Habitats: done")

    crates_placed = 0
    attempts = 0
    while crates_placed < 15 and attempts < 500:
        attempts += 1
        cx = random.randint(2, GRID_WIDTH - 3)
        cy = random.randint(2, GRID_HEIGHT - 3)
        if tiles.get((cx, cy)) == "gravel":
            CreateTile(cx, cy, "crate")
            crate_contents[(cx, cy)] = {
                "energy": random.randint(50, 150),
                "opened": False,
            }
            crates_placed += 1
            total += 1
    print(f"  Crates: placed {crates_placed} crates")

    elapsed = time.time() - t0
    print(f"Procedural map complete: {total} non-gravel tiles in {elapsed:.3f}s")


def _initialize_default_tiles() -> None:
    with tiles_lock:
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                tiles[(x, y)] = "gravel"
                tile_matrix[x][y] = Tile(
                    x=x,
                    y=y,
                    type="gravel",
                    color=TILE_COLORS["gravel"],
                    description=TILE_DESCRIPTIONS["gravel"],
                    fog=enable_fog_of_war,
                )


def initialize_world(use_fog: bool = True) -> None:
    global enable_fog_of_war
    enable_fog_of_war = use_fog
    _initialize_default_tiles()
    _build_scenery_procedural()


def CreateTile(x: int, y: int, type: str) -> dict[str, Any]:
    if type not in TILE_TYPES:
        return {"ok": False, "error": f"Unknown tile type: {type}"}
    if not (0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT):
        return {"ok": False, "error": "Tile out of bounds"}

    color = TILE_COLORS[type]
    description = TILE_DESCRIPTIONS[type]

    with tiles_lock:
        existing_fog = (
            tile_matrix[x][y].fog if (0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT) else True
        )
        tiles[(x, y)] = type
        tile_matrix[x][y] = Tile(
            x=x,
            y=y,
            type=type,
            color=color,
            description=description,
            fog=existing_fog,
        )

    return {"ok": True, "x": x, "y": y, "type": type}


def _consume_energy(amount: int = 1) -> None:
    global bot_energy
    bot_energy = max(0, bot_energy - amount)


def _advance_solar_flare_step(current_step: int | None = None) -> bool:
    """Advance the solar flare countdown and resolve effects.

    Returns True if the bot survives this step, False if destroyed.
    """
    global STEPS_TO_SOLAR_FLARE, bot_energy, bot_state
    global solar_flare_animation_active, solar_flare_animation_start_time
    global solar_flare_last_step
    step = current_step if current_step is not None else bot_step_count
    if step > 0 and step == solar_flare_last_step:
        return True
    if step > 0:
        solar_flare_last_step = step
    STEPS_TO_SOLAR_FLARE -= 1
    if STEPS_TO_SOLAR_FLARE > 0:
        return True

    # Solar flare occurs - destroy all habitats and drain energy (unless protected)
    print("\n  [SolarFlare] === SOLAR FLARE EVENT ===")
    bot_gx, bot_gy = _bot_grid_pos()
    bot_in_habitat = tile_matrix[bot_gx][bot_gy].type == "habitat"
    habitats_destroyed = 0
    for (hx, hy) in list(habitat_damage.keys()):
        if tile_matrix[hx][hy].type == "habitat":
            CreateTile(hx, hy, "gravel")
            del habitat_damage[(hx, hy)]
            habitats_destroyed += 1
    print(f"  [SolarFlare] Destroyed {habitats_destroyed} habitat(s)!")

    # Drain 100 energy from bot unless sheltered in a habitat at flare time
    if bot_in_habitat:
        print("  [SolarFlare] Bot was sheltered in a habitat - no energy drain.")
    else:
        energy_before = bot_energy
        bot_energy = max(0, bot_energy - 100)
        print(f"  [SolarFlare] Drained 100 energy from bot ({energy_before} -> {bot_energy})")
    
    # Trigger flash animation
    solar_flare_animation_active = True
    solar_flare_animation_start_time = time.time()
    
    # Reset countdown
    STEPS_TO_SOLAR_FLARE = STEPS_SOLAR_FLARE_EVERY
    
    # Bot survives (unless energy runs out)
    if bot_energy <= 0:
        bot_state = "Destroyed"
        print("  [SolarFlare] Bot destroyed - out of energy!")
        return False
    
    return True


def _bot_grid_pos() -> tuple[int, int]:
    gx = max(0, min(GRID_WIDTH - 1, int(bot_target_x) // TILE_SIZE))
    gy = max(0, min(GRID_HEIGHT - 1, int(bot_target_y) // TILE_SIZE))
    return gx, gy


def MoveTo(target_x: int, target_y: int) -> dict[str, Any]:
    global bot_target_x, bot_target_y
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
    target_x = max(0, min(GRID_WIDTH - 1, int(target_x)))
    target_y = max(0, min(GRID_HEIGHT - 1, int(target_y)))

    start_gx, start_gy = _bot_grid_pos()

    if start_gx == target_x and start_gy == target_y:
        print(f"  [MoveTo] Already at target ({target_x}, {target_y})")
        return {
            "ok": True,
            "target_tile_x": target_x,
            "target_tile_y": target_y,
            "steps_taken": 0,
            "tile_x": start_gx,
            "tile_y": start_gy,
            "tile_type": tile_matrix[start_gx][start_gy].type,
            "energy": bot_energy,
            "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
        }

    curr_gx, curr_gy = start_gx, start_gy

    # Enable diagonal movement for efficient travel
    dx = 0
    dy = 0
    if curr_gx != target_x:
        dx = 1 if target_x > curr_gx else -1
    if curr_gy != target_y:
        dy = 1 if target_y > curr_gy else -1

    start_tile = tile_matrix[start_gx][start_gy]
    terrain_limit = TILE_MAX_DISTANCE.get(start_tile.type, 5)
    
    # Check if terrain is impassable
    if terrain_limit <= 0:
        msg = (
            f"Cannot move — stuck on {start_tile.type} at ({start_gx}, {start_gy})! "
            f"Terrain is impassable."
        )
        print(f"  [MoveTo] BLOCKED: {msg}")
        return {
            "ok": False,
            "error": msg,
            "energy": bot_energy,
            "tile_x": start_gx,
            "tile_y": start_gy,
            "tile_type": start_tile.type,
        }

    steps_taken = 0
    new_x, new_y = bot_target_x, bot_target_y
    for _ in range(min(10, terrain_limit)):
        _consume_energy(1)
        next_x = new_x + dx * TILE_SIZE
        next_y = new_y + dy * TILE_SIZE
        next_x = max(BOT_RADIUS, min(MAP_WIDTH - BOT_RADIUS, next_x))
        next_y = max(BOT_RADIUS, min(MAP_HEIGHT - BOT_RADIUS, next_y))

        next_gx = max(0, min(GRID_WIDTH - 1, int(next_x) // TILE_SIZE))
        next_gy = max(0, min(GRID_HEIGHT - 1, int(next_y) // TILE_SIZE))
        next_tile = tile_matrix[next_gx][next_gy]
        next_tile.fog = False

        new_x = next_x
        new_y = next_y
        steps_taken += 1

        # Check if we've reached the target (both X and Y aligned)
        if next_gx == target_x and next_gy == target_y:
            print(f"  [MoveTo] Reached target ({target_x}, {target_y}) in {steps_taken} steps")
            break
        
        # Update direction if one axis is aligned (for diagonal->orthogonal transition)
        if next_gx == target_x:
            dx = 0
        if next_gy == target_y:
            dy = 0

    bot_target_x = new_x
    bot_target_y = new_y

    grid_x, grid_y = _bot_grid_pos()
    landed = tile_matrix[grid_x][grid_y]

    print(
        f"  [MoveTo] From ({start_gx}, {start_gy}) toward ({target_x}, {target_y}), "
        f"took {steps_taken} steps → ({grid_x}, {grid_y}) = {landed.type}"
    )

    return {
        "ok": True,
        "target_tile_x": target_x,
        "target_tile_y": target_y,
        "steps_taken": steps_taken,
        "terrain_limit": terrain_limit,
        "tile_x": grid_x,
        "tile_y": grid_y,
        "tile_type": landed.type,
        "tile_description": landed.description,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def LookClose() -> dict[str, Any]:
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
    _consume_energy(1)
    grid_x, grid_y = _bot_grid_pos()

    surrounding: list[dict[str, Any]] = []
    with tiles_lock:
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                nx, ny = grid_x + dx, grid_y + dy
                if 0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT:
                    t = tile_matrix[nx][ny]
                    t.fog = False
                    label = "center" if dx == 0 and dy == 0 else ""
                    tile_info = {
                        "x": t.x,
                        "y": t.y,
                        "type": t.type,
                        "description": t.description,
                        "position": label,
                    }
                    # Add habitat damage info if it's a habitat
                    if t.type == "habitat":
                        habitat = habitat_damage.get((nx, ny))
                        if habitat:
                            tile_info["damage"] = habitat["damage"]
                            tile_info["repaired"] = habitat["repaired"]
                    surrounding.append(tile_info)

    print(
        f"  [LookClose] Bot at tile ({grid_x}, {grid_y}), "
        f"scanned {len(surrounding)} surrounding tiles"
    )

    # LookClose action completed - sprite change is handled by TOOL_STATE

    return {
        "ok": True,
        "bot_tile_x": grid_x,
        "bot_tile_y": grid_y,
        "surrounding": surrounding,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def _is_line_of_sight_blocked(from_x: int, from_y: int, to_x: int, to_y: int) -> tuple[bool, tuple[int, int] | None]:
    """Check if line of sight is blocked. Returns (is_blocked, blocking_tile_coords)."""
    dx = abs(to_x - from_x)
    dy = abs(to_y - from_y)
    sx = 1 if from_x < to_x else -1
    sy = 1 if from_y < to_y else -1

    x, y = from_x, from_y
    err = dx - dy

    while True:
        if (x, y) != (from_x, from_y):
            if 0 <= x < GRID_WIDTH and 0 <= y < GRID_HEIGHT:
                if tile_matrix[x][y].type in {"rocks", "habitat"}:
                    tile_matrix[x][y].fog = False
                    return True, (x, y)

        if x == to_x and y == to_y:
            break

        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x += sx
        if e2 < dx:
            err += dx
            y += sy

    return False, None


def LookFar() -> dict[str, Any]:
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    radius = bot_lookfar_distance

    features: list[dict[str, Any]] = []
    blocking_tiles_added: set[tuple[int, int]] = set()
    
    with tiles_lock:
        # Iterate over tiles within the bounding box of the circle
        for tx in range(max(0, gx - radius), min(GRID_WIDTH, gx + radius + 1)):
            for ty in range(max(0, gy - radius), min(GRID_HEIGHT, gy + radius + 1)):
                # Check if this tile is actually within the circle (Euclidean distance)
                dx = tx - gx
                dy = ty - gy
                euclidean_dist = (dx * dx + dy * dy) ** 0.5
                
                if euclidean_dist > radius:
                    continue
                
                t = tile_matrix[tx][ty]
                time.sleep(0.001)
                
                is_blocked, blocking_pos = _is_line_of_sight_blocked(gx, gy, tx, ty)
                
                # If blocked, add the blocking tile (if not already added)
                if is_blocked and blocking_pos and blocking_pos not in blocking_tiles_added:
                    bx, by = blocking_pos
                    blocking_tile = tile_matrix[bx][by]
                    blocking_dist = ((bx - gx) ** 2 + (by - gy) ** 2) ** 0.5
                    blocking_info = {
                        "type": blocking_tile.type,
                        "distance": blocking_dist,
                        "x": bx,
                        "y": by,
                    }
                    # Add habitat damage info if it's a habitat
                    if blocking_tile.type == "habitat":
                        habitat = habitat_damage.get((bx, by))
                        if habitat:
                            blocking_info["damage"] = habitat["damage"]
                            blocking_info["repaired"] = habitat["repaired"]
                    features.append(blocking_info)
                    blocking_tiles_added.add(blocking_pos)
                    continue  # Skip the tile behind the blocker
                
                if is_blocked:
                    continue  # Skip tiles behind blockers
                
                # Add visible tile
                t.fog = False
                feature_info = {
                    "type": t.type,
                    "distance": euclidean_dist,
                    "x": tx,
                    "y": ty,
                }
                # Add habitat damage info if it's a habitat
                if t.type == "habitat":
                    habitat = habitat_damage.get((tx, ty))
                    if habitat:
                        feature_info["damage"] = habitat["damage"]
                        feature_info["repaired"] = habitat["repaired"]
                features.append(feature_info)

    best: dict[str, dict[str, Any]] = {}
    for f in features:
        key = f["type"]
        if key not in best or f["distance"] < best[key]["distance"]:
            best[key] = f
    summary = sorted(best.values(), key=lambda item: item["distance"])

    print(f"  [LookFar] Scanned circle radius {radius} from ({gx}, {gy}): found {len(summary)} notable features")
    for f in summary:
        damage_info = f" [damage: {f['damage']}%]" if f.get("damage") is not None else ""
        print(f"    {f['type']:>7} at ({f['x']}, {f['y']}) dist={f['distance']:.1f}{damage_info}")

    # Hold the LookFar state for 1 second so user sees the sprite
    start_time = time.time()
    while time.time() - start_time < 1.0:
        time.sleep(0.05)

    return {
        "ok": True,
        "bot_tile_x": gx,
        "bot_tile_y": gy,
        "features": summary,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def OpenCrate() -> dict[str, Any]:
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
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
        return {
            "ok": True,
            "already_opened": True,
            "energy_inside": crate["energy"],
            "energy": bot_energy,
        }

    crate["opened"] = True
    print(f"  [OpenCrate] Opened crate at ({gx}, {gy}) — contains {crate['energy']} energy!")
    return {
        "ok": True,
        "already_opened": False,
        "energy_inside": crate["energy"],
        "tile_x": gx,
        "tile_y": gy,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def TakeAllFromCrate() -> dict[str, Any]:
    global bot_energy
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
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

    CreateTile(gx, gy, "gravel")
    del crate_contents[(gx, gy)]

    print(f"  [TakeAllFromCrate] Took {gained} energy from crate at ({gx}, {gy}). Bot energy now: {bot_energy}")
    time.sleep(gained / 10)
    return {
        "ok": True,
        "energy_gained": gained,
        "energy": bot_energy,
        "tile_x": gx,
        "tile_y": gy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def Dig() -> dict[str, Any]:
    global bot_inventory
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
    
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    
    if tile_matrix[gx][gy].type != "rocks":
        msg = f"No rocks at tile ({gx}, {gy}). Current tile: {tile_matrix[gx][gy].type}."
        print(f"  [Dig] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}
    
    # Replace rocks with gravel
    CreateTile(gx, gy, "gravel")
    
    # Add rock to inventory
    bot_inventory.append({"type": "rock"})
    rock_count = sum(1 for item in bot_inventory if item["type"] == "rock")
    habitats_possible = rock_count // ROCKS_REQUIRED_FOR_HABITAT
    
    print(f"  [Dig] Dug a rock at ({gx}, {gy})! Rocks in inventory: {rock_count} (can build {habitats_possible} habitat(s))")
    return {
        "ok": True,
        "rock_obtained": True,
        "rocks_in_inventory": rock_count,
        "habitats_buildable": habitats_possible,
        "tile_x": gx,
        "tile_y": gy,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def CreateHabitat() -> dict[str, Any]:
    global bot_inventory
    if not _advance_solar_flare_step():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}

    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    current_tile = tile_matrix[gx][gy].type

    if current_tile == "habitat":
        msg = f"Tile ({gx}, {gy}) is already a habitat."
        print(f"  [CreateHabitat] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    if current_tile in {"water", "crate"}:
        msg = f"Cannot build habitat on {current_tile} tile at ({gx}, {gy})."
        print(f"  [CreateHabitat] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    rock_count = sum(1 for item in bot_inventory if item.get("type") == "rock")
    if rock_count < ROCKS_REQUIRED_FOR_HABITAT:
        msg = (
            f"Need {ROCKS_REQUIRED_FOR_HABITAT} rocks to build a habitat. "
            f"Current rocks: {rock_count}."
        )
        print(f"  [CreateHabitat] {msg}")
        return {
            "ok": False,
            "error": msg,
            "rocks_in_inventory": rock_count,
            "rocks_needed": ROCKS_REQUIRED_FOR_HABITAT - rock_count,
            "energy": bot_energy,
        }

    rocks_to_consume = ROCKS_REQUIRED_FOR_HABITAT
    new_inventory: list[dict[str, Any]] = []
    for item in bot_inventory:
        if rocks_to_consume > 0 and item.get("type") == "rock":
            rocks_to_consume -= 1
            continue
        new_inventory.append(item)
    bot_inventory = new_inventory

    CreateTile(gx, gy, "habitat")
    habitat_damage[(gx, gy)] = {"damage": 0, "repaired": True}
    tile_matrix[gx][gy].color = TILE_COLORS["habitat"]

    rocks_left = sum(1 for item in bot_inventory if item.get("type") == "rock")
    print(
        f"  [CreateHabitat] Built habitat at ({gx}, {gy}) using {ROCKS_REQUIRED_FOR_HABITAT} rocks. "
        f"Rocks left: {rocks_left}"
    )
    return {
        "ok": True,
        "habitat_created": True,
        "tile_x": gx,
        "tile_y": gy,
        "rocks_consumed": ROCKS_REQUIRED_FOR_HABITAT,
        "rocks_in_inventory": rocks_left,
        "energy": bot_energy,
        "steps_to_solar_flare": STEPS_TO_SOLAR_FLARE,
    }


def get_tool_dispatch() -> dict[str, Any]:
    return {
        "MoveTo": MoveTo,
        "LookClose": LookClose,
        "LookFar": LookFar,
        "OpenCrate": OpenCrate,
        "TakeAllFromCrate": TakeAllFromCrate,
        "Dig": Dig,
        "CreateHabitat": CreateHabitat,
    }


def print_step_status() -> None:
    gx, gy = _bot_grid_pos()
    habitats_total = len(habitat_damage)
    print("\n  === STATUS ===")
    print(f"  Energy: {bot_energy}  |  Position: tile ({gx}, {gy})  |  Solar flare in: {STEPS_TO_SOLAR_FLARE} steps")
    print(f"  Habitats existing: {habitats_total}")
    print(f"  Inventory: {bot_inventory if bot_inventory else '(empty)'}")
    print("  Surroundings:")
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
    print("  ==============\n")


def update(dt: float) -> None:
    global bot_x, bot_y, bot_target_x, bot_target_y, bot_state

    dx = 0
    dy = 0

    if dx or dy:
        bot_target_x += dx * BOT_SPEED * dt
        bot_target_y += dy * BOT_SPEED * dt
        bot_target_x = max(BOT_RADIUS, min(MAP_WIDTH - BOT_RADIUS, bot_target_x))
        bot_target_y = max(BOT_RADIUS, min(MAP_HEIGHT - BOT_RADIUS, bot_target_y))

    move_speed = TILE_SIZE * 2
    diff_x = bot_target_x - bot_x
    diff_y = bot_target_y - bot_y
    dist = (diff_x**2 + diff_y**2) ** 0.5

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
