import importlib.resources
from typing import Any

import pygame


_SPRITE_SHEET: pygame.Surface | None = None
_SPRITE_SIZE = 150
_STATE_SPRITE_POS: dict[str, tuple[int, int]] = {
    "Waiting": (0, 0),
    "Thinking": (1, 0),
    "Moving": (2, 0),
    "LookClose": (0, 1),
    "LookFar": (1, 1),
    "Charging": (2, 1),
}


def draw_game(screen: Any, Rect: Any, game_logic: Any, ollama_model: str) -> None:
    global _SPRITE_SHEET

    screen.clear()

    gx, gy = game_logic._bot_grid_pos()
    cam_tile_x = gx
    cam_tile_y = gy

    half_w = game_logic.VIEWPORT_TILES_W // 2
    half_h = game_logic.VIEWPORT_TILES_H // 2
    tile_x_start = max(0, cam_tile_x - half_w)
    tile_x_end = min(game_logic.GRID_WIDTH, cam_tile_x + half_w + 1)
    tile_y_start = max(0, cam_tile_y - half_h)
    tile_y_end = min(game_logic.GRID_HEIGHT, cam_tile_y + half_h + 1)

    cam_world_x = game_logic.bot_x
    cam_world_y = game_logic.bot_y

    with game_logic.tiles_lock:
        for tx in range(tile_x_start, tile_x_end):
            for ty in range(tile_y_start, tile_y_end):
                t = game_logic.tile_matrix[tx][ty]
                color = (60, 60, 60) if t.fog else t.color
                world_x = tx * game_logic.TILE_SIZE
                world_y = ty * game_logic.TILE_SIZE
                screen_x = (
                    (world_x - cam_world_x) * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE)
                    + game_logic.MAP_WIDTH / 2
                )
                screen_y = (
                    (world_y - cam_world_y) * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE)
                    + game_logic.MAP_HEIGHT / 2
                )
                screen.draw.filled_rect(
                    Rect((int(screen_x), int(screen_y)), (game_logic.DRAW_TILE_SIZE, game_logic.DRAW_TILE_SIZE)),
                    color,
                )

    if _SPRITE_SHEET is None:
        try:
            resource = importlib.resources.files("bots.resources").joinpath("bots.png")
            with importlib.resources.as_file(resource) as sheet_path:
                _SPRITE_SHEET = pygame.image.load(str(sheet_path)).convert_alpha()
            print("  [Sprite] Loaded spritesheet from package resources")
        except Exception as exc:
            print(f"  [Sprite] Failed to load bots.png: {exc}")
            radius_scaled = int(game_logic.BOT_RADIUS * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE))
            screen.draw.filled_circle(
                (game_logic.MAP_WIDTH // 2, game_logic.MAP_HEIGHT // 2),
                radius_scaled,
                (0, 120, 255),
            )
            return

    col, row = _STATE_SPRITE_POS.get(game_logic.bot_state, (0, 0))
    src_rect = pygame.Rect(
        col * _SPRITE_SIZE,
        row * _SPRITE_SIZE,
        _SPRITE_SIZE,
        _SPRITE_SIZE,
    )
    sprite = _SPRITE_SHEET.subsurface(src_rect)

    draw_size = int(game_logic.BOT_RADIUS * 6 * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE))
    scaled = pygame.transform.smoothscale(sprite, (draw_size, draw_size))

    dest_x = int(game_logic.MAP_WIDTH / 2) - draw_size // 2
    dest_y = int(game_logic.MAP_HEIGHT / 2) - draw_size // 2
    screen.surface.blit(scaled, (dest_x, dest_y))

    sidebar_x = game_logic.MAP_WIDTH
    screen.draw.filled_rect(
        Rect((sidebar_x, 0), (game_logic.SIDEBAR_WIDTH, game_logic.MAP_HEIGHT)),
        (20, 20, 30),
    )
    screen.draw.line((sidebar_x, 0), (sidebar_x, game_logic.MAP_HEIGHT), (60, 60, 80))

    pad = 10
    font_stats = pygame.font.SysFont("monospace", 16, bold=True)
    stat_lh = font_stats.get_linesize()
    sx = sidebar_x + pad
    sy = pad

    gx, gy = game_logic._bot_grid_pos()
    tgx = max(0, min(game_logic.GRID_WIDTH - 1, int(game_logic.bot_target_x) // game_logic.TILE_SIZE))
    tgy = max(0, min(game_logic.GRID_HEIGHT - 1, int(game_logic.bot_target_y) // game_logic.TILE_SIZE))

    stat_lines = [
        f"Energy: {game_logic.bot_energy}",
        f"Pos: ({gx}, {gy})",
        f"Target: ({tgx}, {tgy})",
        f"State: {game_logic.bot_state}",
        "",
        "Model:",
        f" {ollama_model}",
    ]
    for i, line in enumerate(stat_lines):
        color = (180, 220, 255) if i < 4 else (140, 160, 200)
        surf = font_stats.render(line, True, color)
        screen.surface.blit(surf, (sx, sy + i * stat_lh))

    panel_y = game_logic.MAP_HEIGHT
    screen.draw.filled_rect(
        Rect((0, panel_y), (game_logic.WIDTH, game_logic.PANEL_HEIGHT)),
        (20, 20, 30),
    )
    screen.draw.line((0, panel_y), (game_logic.WIDTH, panel_y), (60, 60, 80))

    padding = 14
    font_speech = pygame.font.SysFont("monospace", 22)

    if game_logic.bot_last_speech:
        speech_y = panel_y + padding
        line_height = font_speech.get_linesize()
        max_width = game_logic.WIDTH - padding * 2 - 30

        words = game_logic.bot_last_speech.split()
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

        remaining = game_logic.PANEL_HEIGHT - padding * 2
        max_lines = max(1, remaining // line_height)
        if len(lines) > max_lines:
            lines = lines[:max_lines]
            lines[-1] = lines[-1][:-3] + "..." if len(lines[-1]) > 3 else "..."

        for i, line in enumerate(lines):
            prefix = "🤖 " if i == 0 else "   "
            text_surface = font_speech.render(f"{prefix}{line}", True, (220, 220, 220))
            screen.surface.blit(text_surface, (padding, speech_y + i * line_height))
