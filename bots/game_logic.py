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
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "ministral-3:3b")
#OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen2.5-coder:latest")


OLLAMA_PLAY = os.getenv("OLLAMA_PLAY", "0") == "1"

TILE_SIZE = 4
GRID_WIDTH = MAP_WIDTH // TILE_SIZE
GRID_HEIGHT = MAP_HEIGHT // TILE_SIZE

VIEWPORT_TILES_W = 70
VIEWPORT_TILES_H = 70
DRAW_TILE_SIZE = MAP_WIDTH // VIEWPORT_TILES_W

BOT_RADIUS = 10
BOT_SPEED = 220
MOVE_MAX_TILES = 20
HOURS_SOLAR_FLARE_EVERY = 50
HOURS_TO_SOLAR_FLARE = HOURS_SOLAR_FLARE_EVERY
ROCKS_REQUIRED_FOR_HABITAT = 1
ROCKS_REQUIRED_FOR_BATTERY = 1
ROCKS_REQUIRED_FOR_SOLAR_PANEL = 1
HABITAT_SOLAR_CHARGE = 25

BUILDABLE_TILE_TYPES = {"habitat", "battery", "solar_panel"}
BUILD_COSTS: dict[str, int] = {
    "habitat": ROCKS_REQUIRED_FOR_HABITAT,
    "battery": ROCKS_REQUIRED_FOR_BATTERY,
    "solar_panel": ROCKS_REQUIRED_FOR_SOLAR_PANEL,
}
POWER_NETWORK_TILE_TYPES = {"habitat", "battery", "solar_panel"}

# Solar flare animation state
solar_flare_animation_active = False
solar_flare_animation_start_time = 0.0
solar_flare_last_hour = -1
charging_animation_until = 0.0

TILE_TYPES = {
    "gravel",
    "sand",
    "water",
    "rocks",
    "habitat",
    "battery",
    "solar_panel",
}
TILE_COLORS = {
    "gravel": (140, 120, 100),
    "sand": (220, 200, 120),
    "water": (120, 210, 255),
    "rocks": (130, 130, 130),
    "habitat": (180, 255, 100),
    "battery": (245, 215, 90),
    "solar_panel": (90, 150, 215),
    "broken_habitat": (40, 70, 40),
}

TILE_DESCRIPTIONS = {
    "gravel": "Loose red gravel and dust.",
    "sand": "Warm, loose sand.",
    "water": "Clear, shimmering water.",
    "rocks": "Jagged rocks and boulders.",
    "habitat": "A sealed habitat module.",
    "battery": "Battery segment connecting habitats and solar panels.",
    "solar_panel": "A solar panel array that powers the settlement.",
}

@dataclass
class Tile:
    x: int
    y: int
    type: str
    color: tuple[int, int, int] = field(default=(80, 170, 80))
    description: str = field(default="Loose red gravel and dust.")
    fog: bool = field(default=True)
    powered: bool = field(default=False)


bot_start_energy = 300
bot_x = 400
bot_y = 550
bot_target_x: float = bot_x
bot_target_y: float = bot_y
bot_energy = bot_start_energy
bot_inventory: list[dict[str, Any]] = []
bot_state: str = "Waiting"
bot_last_speech: str = ""
bot_lookfar_distance = 40
bot_hour_count: int = 0
world_rocks_target: int = 100


tiles: dict[tuple[int, int], str] = {}
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
    "Dig": "LookClose",
    "Create": "LookClose",
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


def _build_scenery_procedural(rocks_target: int = 100) -> None:
    t0 = time.time()
    total = 0
    print("Generating map procedurally...")

    # Original terrain tuning was authored for 8px tiles.
    terrain_scale = 8 / TILE_SIZE

    def _scaled(v: int) -> int:
        return max(1, int(round(v * terrain_scale)))

    # Scale biome counts by map area relative to the original 8px-tile tuning.
    area_scale = terrain_scale * terrain_scale

    rock_fields = max(1, int(rocks_target))
    rock_total = 0
    for _ in range(rock_fields):
        cx = random.randint(6, GRID_WIDTH - 7)
        cy = random.randint(6, GRID_HEIGHT - 7)
        radius = random.randint(_scaled(1), _scaled(2))
        rock_total += _place_rock_field(cx, cy, radius, density=random.uniform(0.85, 1.1))
    total += rock_total
    print(f"  Rocks: target={rocks_target}, fields={rock_fields}, placed={rock_total}")

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


def initialize_world(use_fog: bool = True, rocks_target: int = 100) -> None:
    global enable_fog_of_war, world_rocks_target
    enable_fog_of_war = use_fog
    world_rocks_target = max(1, int(rocks_target))
    _initialize_default_tiles()
    _build_scenery_procedural(world_rocks_target)


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
            powered=False,
        )

    return {"ok": True, "x": x, "y": y, "type": type}


def _consume_energy(amount: int = 1) -> None:
    global bot_energy
    bot_energy = max(0, bot_energy - amount)


def _advance_solar_flare_hour(current_hour: int | None = None) -> bool:
    """Advance the solar flare countdown and resolve effects.

    # Returns True if the bot survives this hour, False if destroyed.
    """
    global HOURS_TO_SOLAR_FLARE, bot_energy, bot_state
    global solar_flare_animation_active, solar_flare_animation_start_time
    global solar_flare_last_hour
    hour = current_hour if current_hour is not None else bot_hour_count
    if hour > 0 and hour == solar_flare_last_hour:
        return True
    if hour > 0:
        solar_flare_last_hour = hour

    _apply_powered_habitat_charge()

    HOURS_TO_SOLAR_FLARE -= 1
    if HOURS_TO_SOLAR_FLARE > 0:
        return True

    # Solar flare occurs - habitats provide shelter from energy drain.
    print("\n  [SolarFlare] === SOLAR FLARE EVENT ===")
    bot_gx, bot_gy = _bot_grid_pos()
    if tile_matrix[bot_gx][bot_gy].type == "habitat":
        print("  [SolarFlare] Bot is inside habitat - protected from flare drain.")
    else:
        energy_before = bot_energy
        bot_energy = max(0, bot_energy - 100)
        print(f"  [SolarFlare] Drained 100 energy from bot ({energy_before} -> {bot_energy})")
    
    # Trigger flash animation
    solar_flare_animation_active = True
    solar_flare_animation_start_time = time.time()
    
    # Reset countdown
    HOURS_TO_SOLAR_FLARE = HOURS_SOLAR_FLARE_EVERY
    
    # Bot survives (unless energy runs out)
    if bot_energy <= 0:
        bot_state = "Destroyed"
        print("  [SolarFlare] Bot destroyed - out of energy!")
        return False
    
    return True


def _recompute_power_network() -> None:
    """Recompute cached power state for all habitat/battery/solar tiles."""
    with tiles_lock:
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                if tile_matrix[x][y].type in POWER_NETWORK_TILE_TYPES:
                    tile_matrix[x][y].powered = False

        visited: set[tuple[int, int]] = set()
        for x in range(GRID_WIDTH):
            for y in range(GRID_HEIGHT):
                if (x, y) in visited:
                    continue
                if tile_matrix[x][y].type not in POWER_NETWORK_TILE_TYPES:
                    continue

                component: list[tuple[int, int]] = []
                queue_nodes: list[tuple[int, int]] = [(x, y)]
                visited.add((x, y))
                has_battery = False
                has_solar_panel = False

                i = 0
                while i < len(queue_nodes):
                    cx, cy = queue_nodes[i]
                    i += 1
                    component.append((cx, cy))
                    tile_type = tile_matrix[cx][cy].type
                    if tile_type == "battery":
                        has_battery = True
                    elif tile_type == "solar_panel":
                        has_solar_panel = True

                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        nx, ny = cx + dx, cy + dy
                        if not (0 <= nx < GRID_WIDTH and 0 <= ny < GRID_HEIGHT):
                            continue
                        if (nx, ny) in visited:
                            continue
                        if tile_matrix[nx][ny].type not in POWER_NETWORK_TILE_TYPES:
                            continue
                        visited.add((nx, ny))
                        queue_nodes.append((nx, ny))

                component_powered = has_battery and has_solar_panel
                for px, py in component:
                    tile_matrix[px][py].powered = component_powered


def _is_habitat_connected_to_solar(hx: int, hy: int) -> bool:
    """Return cached power status for a habitat tile."""
    if not (0 <= hx < GRID_WIDTH and 0 <= hy < GRID_HEIGHT):
        return False
    tile = tile_matrix[hx][hy]
    return tile.type == "habitat" and tile.powered


def _apply_powered_habitat_charge() -> None:
    """If bot is in a powered habitat, recharge it each hour tick."""
    global bot_energy, bot_state, charging_animation_until
    gx, gy = _bot_grid_pos()
    tile = tile_matrix[gx][gy]
    if tile.type != "habitat" or not tile.powered:
        return
    before = bot_energy
    bot_energy += HABITAT_SOLAR_CHARGE
    bot_state = "Charging"
    charging_animation_until = time.time() + 0.8
    print(
        f"  [Power] Habitat at ({gx}, {gy}) is connected to solar panel via battery: "
        f"+{HABITAT_SOLAR_CHARGE} energy ({before} -> {bot_energy})"
    )


def _bot_grid_pos() -> tuple[int, int]:
    gx = max(0, min(GRID_WIDTH - 1, int(bot_target_x) // TILE_SIZE))
    gy = max(0, min(GRID_HEIGHT - 1, int(bot_target_y) // TILE_SIZE))
    return gx, gy


def MoveTo(target_x: int, target_y: int) -> dict[str, Any]:
    global bot_target_x, bot_target_y
    if not _advance_solar_flare_hour():
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
            "hours_taken": 0,
            "tile_x": start_gx,
            "tile_y": start_gy,
            "tile_type": tile_matrix[start_gx][start_gy].type,
            "energy": bot_energy,
            "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
        }

    curr_gx, curr_gy = start_gx, start_gy

    # Enable diagonal movement for efficient travel
    dx = 0
    dy = 0
    if curr_gx != target_x:
        dx = 1 if target_x > curr_gx else -1
    if curr_gy != target_y:
        dy = 1 if target_y > curr_gy else -1

    hours_taken = 0
    new_x, new_y = bot_target_x, bot_target_y
    for _ in range(MOVE_MAX_TILES):
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
        hours_taken += 1

        # Check if we've reached the target (both X and Y aligned)
        if next_gx == target_x and next_gy == target_y:
            print(f"  [MoveTo] Reached target ({target_x}, {target_y}) in {hours_taken} hours")
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
        f"took {hours_taken} hours → ({grid_x}, {grid_y}) = {landed.type}"
    )

    return {
        "ok": True,
        "target_tile_x": target_x,
        "target_tile_y": target_y,
        "hours_taken": hours_taken,
        "move_limit": MOVE_MAX_TILES,
        "tile_x": grid_x,
        "tile_y": grid_y,
        "tile_type": landed.type,
        "tile_description": landed.description,
        "energy": bot_energy,
        "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
    }


def LookClose() -> dict[str, Any]:
    if not _advance_solar_flare_hour():
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
                    if t.type in POWER_NETWORK_TILE_TYPES:
                        tile_info["powered"] = t.powered
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
        "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
    }


def _is_line_of_sight_blocked(
    from_x: int,
    from_y: int,
    to_x: int,
    to_y: int,
    tile_types: list[list[str]] | None = None,
) -> tuple[bool, tuple[int, int] | None]:
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
                tile_type = tile_types[x][y] if tile_types is not None else tile_matrix[x][y].type
                if tile_type in {"rocks", "habitat"}:
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
    if not _advance_solar_flare_hour():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}
    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    radius = bot_lookfar_distance

    features: list[dict[str, Any]] = []
    visible_type_counts: dict[str, int] = {}
    blocking_tiles_added: set[tuple[int, int]] = set()
    visible_positions: list[tuple[int, int]] = []

    # Snapshot tile types once so expensive LOS work does not hold the world lock.
    with tiles_lock:
        tile_types = [[tile_matrix[x][y].type for y in range(GRID_HEIGHT)] for x in range(GRID_WIDTH)]

    # Iterate over tiles within the bounding box of the circle.
    for tx in range(max(0, gx - radius), min(GRID_WIDTH, gx + radius + 1)):
        for ty in range(max(0, gy - radius), min(GRID_HEIGHT, gy + radius + 1)):
            dx = tx - gx
            dy = ty - gy
            euclidean_dist = (dx * dx + dy * dy) ** 0.5

            if euclidean_dist > radius:
                continue

            is_blocked, blocking_pos = _is_line_of_sight_blocked(gx, gy, tx, ty, tile_types)

            # If blocked, add the blocking tile (if not already added).
            if is_blocked and blocking_pos and blocking_pos not in blocking_tiles_added:
                bx, by = blocking_pos
                blocking_type = tile_types[bx][by]
                blocking_dist = ((bx - gx) ** 2 + (by - gy) ** 2) ** 0.5
                blocking_info = {
                    "type": blocking_type,
                    "distance": blocking_dist,
                    "x": bx,
                    "y": by,
                }
                if blocking_type == "habitat":
                    habitat = habitat_damage.get((bx, by))
                    if habitat:
                        blocking_info["damage"] = habitat["damage"]
                        blocking_info["repaired"] = habitat["repaired"]
                features.append(blocking_info)
                blocking_tiles_added.add(blocking_pos)
                continue

            if is_blocked:
                continue

            visible_positions.append((tx, ty))
            tile_type = tile_types[tx][ty]
            visible_type_counts[tile_type] = visible_type_counts.get(tile_type, 0) + 1

            feature_info = {
                "type": tile_type,
                "distance": euclidean_dist,
                "x": tx,
                "y": ty,
            }
            if tile_type == "habitat":
                habitat = habitat_damage.get((tx, ty))
                if habitat:
                    feature_info["damage"] = habitat["damage"]
                    feature_info["repaired"] = habitat["repaired"]
            if tile_type in POWER_NETWORK_TILE_TYPES:
                feature_info["powered"] = tile_matrix[tx][ty].powered
            features.append(feature_info)

    # Reveal fog for visible tiles in one short critical section.
    with tiles_lock:
        for tx, ty in visible_positions:
            tile_matrix[tx][ty].fog = False

    best: dict[str, dict[str, Any]] = {}
    for f in features:
        key = f["type"]
        if key not in best or f["distance"] < best[key]["distance"]:
            best[key] = f
    summary = sorted(best.values(), key=lambda item: item["distance"])

    print(f"  [LookFar] Scanned circle radius {radius} from ({gx}, {gy}): found {len(summary)} notable features")
    if visible_type_counts:
        counts_text = ", ".join(
            f"{k}:{v}" for k, v in sorted(visible_type_counts.items(), key=lambda item: item[0])
        )
        print(f"    visible counts -> {counts_text}")
    for f in summary:
        damage_info = f" [damage: {f['damage']}%]" if f.get("damage") is not None else ""
        print(f"    {f['type']:>7} at ({f['x']}, {f['y']}) dist={f['distance']:.1f}{damage_info}")

    return {
        "ok": True,
        "bot_tile_x": gx,
        "bot_tile_y": gy,
        "features": summary,
        "visible_type_counts": visible_type_counts,
        "energy": bot_energy,
        "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
    }


def Dig() -> dict[str, Any]:
    global bot_inventory
    if not _advance_solar_flare_hour():
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
        "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
    }


def Create(tile_type: str) -> dict[str, Any]:
    global bot_inventory
    if not _advance_solar_flare_hour():
        return {"ok": False, "error": "Destroyed by solar flare.", "energy": bot_energy}

    _consume_energy(1)
    gx, gy = _bot_grid_pos()
    tile_type = str(tile_type).strip().lower()

    if tile_type not in BUILDABLE_TILE_TYPES:
        msg = (
            f"Unsupported build tile type: '{tile_type}'. "
            f"Allowed values: {sorted(BUILDABLE_TILE_TYPES)}."
        )
        print(f"  [Create] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    current_tile = tile_matrix[gx][gy].type

    if current_tile in BUILDABLE_TILE_TYPES:
        msg = f"You cannot build here. The tile in ({gx}, {gy}) is a {current_tile}."
        print(f"  [Create] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    if current_tile in {"habitat", "battery", "solar_panel"}:
        msg = (
            f"Cannot build {tile_type} on existing {current_tile} at ({gx}, {gy}). "
            "Move to an empty terrain tile first."
        )
        print(f"  [Create] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    if current_tile in {"water"}:
        msg = f"Cannot build {tile_type} on {current_tile} tile at ({gx}, {gy})."
        print(f"  [Create] {msg}")
        return {"ok": False, "error": msg, "energy": bot_energy}

    rocks_required = BUILD_COSTS[tile_type]
    rock_count = sum(1 for item in bot_inventory if item.get("type") == "rock")
    if rock_count < rocks_required:
        msg = (
            f"Need {rocks_required} rocks to build a {tile_type}. "
            f"Current rocks: {rock_count}."
        )
        print(f"  [Create] {msg}")
        return {
            "ok": False,
            "error": msg,
            "rocks_in_inventory": rock_count,
            "rocks_needed": rocks_required - rock_count,
            "energy": bot_energy,
        }

    rocks_to_consume = rocks_required
    new_inventory: list[dict[str, Any]] = []
    for item in bot_inventory:
        if rocks_to_consume > 0 and item.get("type") == "rock":
            rocks_to_consume -= 1
            continue
        new_inventory.append(item)
    bot_inventory = new_inventory

    CreateTile(gx, gy, tile_type)
    if tile_type == "habitat":
        habitat_damage[(gx, gy)] = {"damage": 0, "repaired": True}
        tile_matrix[gx][gy].color = TILE_COLORS["habitat"]
    else:
        habitat_damage.pop((gx, gy), None)
    if tile_type in POWER_NETWORK_TILE_TYPES:
        _recompute_power_network()

    rocks_left = sum(1 for item in bot_inventory if item.get("type") == "rock")
    print(
        f"  [Create] Built {tile_type} at ({gx}, {gy}) using {rocks_required} rocks. "
        f"Rocks left: {rocks_left}"
    )
    return {
        "ok": True,
        "tile_created": tile_type,
        "tile_x": gx,
        "tile_y": gy,
        "rocks_consumed": rocks_required,
        "rocks_in_inventory": rocks_left,
        "energy": bot_energy,
        "hours_to_solar_flare": HOURS_TO_SOLAR_FLARE,
    }


def get_tool_dispatch() -> dict[str, Any]:
    return {
        "MoveTo": MoveTo,
        "LookClose": LookClose,
        "LookFar": LookFar,
        "Dig": Dig,
        "Create": Create,
    }


def print_hour_status() -> None:
    gx, gy = _bot_grid_pos()
    habitats_total = len(habitat_damage)

    if bot_inventory:
        counts: dict[str, int] = {}
        for item in bot_inventory:
            item_type = str(item.get("type", "unknown")) if isinstance(item, dict) else "unknown"
            counts[item_type] = counts.get(item_type, 0) + 1
        inventory_summary = ", ".join(f"{item_type} x{count}" for item_type, count in sorted(counts.items()))
    else:
        inventory_summary = "(empty)"

    print("\n  === STATUS ===")
    print(f"  Energy: {bot_energy}  |  Position: tile ({gx}, {gy})  |  Solar flare in: {HOURS_TO_SOLAR_FLARE} hours")
    print(f"  Habitats existing: {habitats_total}")
    print(f"  Inventory: {inventory_summary}")
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
        hour = move_speed * dt
        if hour >= dist:
            bot_x = bot_target_x
            bot_y = bot_target_y
        else:
            bot_x += (diff_x / dist) * hour
            bot_y += (diff_y / dist) * hour
    elif bot_state == "Moving":
        bot_state = "Waiting"

    if bot_state == "Charging" and time.time() >= charging_animation_until:
        bot_state = "Waiting"
