import importlib.resources
import time
from typing import Any

import pygame
import pygame_gui
from pygame_gui.elements import UIWindow, UITextBox


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

# pygame_gui manager and windows
_ui_manager: pygame_gui.UIManager | None = None
_stats_window: UIWindow | None = None
_log_window: UIWindow | None = None
_speech_window: UIWindow | None = None
_input_window: UIWindow | None = None
_stats_text: UITextBox | None = None
_log_text: UITextBox | None = None
_speech_text: UITextBox | None = None
_last_log_html: str | None = None
_last_speech_html: str | None = None
_last_stats_html: str | None = None


def initialize_ui(screen_size: tuple[int, int], message_log: Any) -> pygame_gui.UIManager:
    """Initialize pygame_gui manager and create the 4 UI windows."""
    global _ui_manager, _stats_window, _log_window, _speech_window, _input_window
    global _stats_text, _log_text, _speech_text
    
    _ui_manager = pygame_gui.UIManager(screen_size)
    
    # 1. Bot Stats Window (top-right)
    _stats_window = UIWindow(
        rect=pygame.Rect((screen_size[0] - 320, 10), (310, 200)),
        manager=_ui_manager,
        window_display_title="Bot Stats",
        resizable=True,
    )
    _stats_text = UITextBox(
        html_text="<font size=4>Initializing...</font>",
        relative_rect=pygame.Rect((0, 0), (290, 160)),
        manager=_ui_manager,
        container=_stats_window,
    )
    
    # 2. Message Log Window (left side)
    _log_window = UIWindow(
        rect=pygame.Rect((10, 10), (400, 300)),
        manager=_ui_manager,
        window_display_title="Message Log",
        resizable=True,
    )
    _log_text = UITextBox(
        html_text="<font size=3>Message log started...</font>",
        relative_rect=pygame.Rect((0, 0), (380, 260)),
        manager=_ui_manager,
        container=_log_window,
    )
    
    # 3. Bot Speech Window (bottom)
    _speech_window = UIWindow(
        rect=pygame.Rect((10, screen_size[1] - 210), (600, 200)),
        manager=_ui_manager,
        window_display_title="Bot Speech",
        resizable=True,
    )
    _speech_text = UITextBox(
        html_text="<font size=4>Waiting for bot to speak...</font>",
        relative_rect=pygame.Rect((0, 0), (580, 160)),
        manager=_ui_manager,
        container=_speech_window,
    )
    
    # 4. User Input Window (bottom-right, non-functional for now)
    _input_window = UIWindow(
        rect=pygame.Rect((screen_size[0] - 420, screen_size[1] - 210), (410, 200)),
        manager=_ui_manager,
        window_display_title="User Input (Coming Soon)",
        resizable=True,
    )
    UITextBox(
        html_text="<font size=4><i>User input functionality coming soon...</i></font>",
        relative_rect=pygame.Rect((0, 0), (390, 160)),
        manager=_ui_manager,
        container=_input_window,
    )
    
    message_log.start_capture()
    return _ui_manager


def update_ui_panels(game_logic: Any, ollama_model: str, message_log: Any) -> None:
    """Update the content of all UI panels."""
    if not _ui_manager:
        return
        
    gx, gy = game_logic._bot_grid_pos()
    tgx = max(0, min(game_logic.GRID_WIDTH - 1, int(game_logic.bot_target_x) // game_logic.TILE_SIZE))
    tgy = max(0, min(game_logic.GRID_HEIGHT - 1, int(game_logic.bot_target_y) // game_logic.TILE_SIZE))
    
    # Update Bot Stats
    if _stats_text:
        stats_html = (
            "<font size=4>"
            f"<b>Energy:</b> {game_logic.bot_energy}<br>"
            f"<b>Position:</b> ({gx}, {gy})<br>"
            f"<b>Target:</b> ({tgx}, {tgy})<br>"
            f"<b>State:</b> {game_logic.bot_state}<br>"
            f"<b>Solar Flare in:</b> {game_logic.STEPS_TO_SOLAR_FLARE}<br>"
            f"<b>Model:</b> {ollama_model}"
            "</font>"
        )
        global _last_stats_html
        if stats_html != _last_stats_html:
            _stats_text.html_text = stats_html
            _stats_text.rebuild()
            _last_stats_html = stats_html
    
    # Update Message Log (last 50 messages)
    if _log_text:
        messages = message_log.get_messages(50)
        log_html = "<font size=3>" + "<br>".join(messages[-50:]) + "</font>"
        global _last_log_html
        if log_html != _last_log_html:
            should_follow = False
            if hasattr(_log_text, "scroll_bar") and _log_text.scroll_bar:
                # Only auto-follow if user is already near the bottom.
                should_follow = _log_text.scroll_bar.scroll_position >= (
                    _log_text.scroll_bar.bottom_limit - 5
                )
            _log_text.html_text = log_html
            _log_text.rebuild()
            _last_log_html = log_html
            if should_follow and _log_text.scroll_bar:
                _log_text.scroll_bar.scroll_position = _log_text.scroll_bar.bottom_limit
    
    # Update Bot Speech
    if _speech_text and game_logic.bot_last_speech:
        speech_html = f'<font size=4>🤖 {game_logic.bot_last_speech}</font>'
        global _last_speech_html
        if speech_html != _last_speech_html:
            _speech_text.html_text = speech_html
            _speech_text.rebuild()
            _last_speech_html = speech_html


def draw_game(screen: Any, Rect: Any, game_logic: Any, ollama_model: str, message_log: Any = None) -> None:
    """Draw the game map and bot sprite (UI windows are drawn separately by pygame_gui)."""
    global _SPRITE_SHEET
    
    # Clear only the map area (not UI)
    screen.surface.fill((0, 0, 0))
    
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

    # Draw tiles
    with game_logic.tiles_lock:
        for tx in range(tile_x_start, tile_x_end):
            for ty in range(tile_y_start, tile_y_end):
                t = game_logic.tile_matrix[tx][ty]
                color = (60, 60, 60) if t.fog else t.color
                world_x = tx * game_logic.TILE_SIZE
                world_y = ty * game_logic.TILE_SIZE
                screen_x = (
                    (world_x - cam_world_x) * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE)
                    + game_logic.WIDTH / 2
                )
                screen_y = (
                    (world_y - cam_world_y) * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE)
                    + game_logic.HEIGHT / 2
                )
                # Only draw if within screen bounds
                if 0 <= screen_x < game_logic.WIDTH and 0 <= screen_y < game_logic.HEIGHT:
                    pygame.draw.rect(
                        screen.surface,
                        color,
                        pygame.Rect(int(screen_x), int(screen_y), game_logic.DRAW_TILE_SIZE, game_logic.DRAW_TILE_SIZE)
                    )

    # Load spritesheet on first draw
    if _SPRITE_SHEET is None:
        try:
            resource = importlib.resources.files("bots.resources").joinpath("bots.png")
            with importlib.resources.as_file(resource) as sheet_path:
                _SPRITE_SHEET = pygame.image.load(str(sheet_path)).convert_alpha()
            print("  [Sprite] Loaded spritesheet from package resources")
        except Exception as exc:
            print(f"  [Sprite] Failed to load bots.png: {exc}")
            radius_scaled = int(game_logic.BOT_RADIUS * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE))
            pygame.draw.circle(
                screen.surface,
                (0, 120, 255),
                (game_logic.WIDTH // 2, game_logic.HEIGHT // 2),
                radius_scaled,
            )
            return

    # Draw bot sprite at screen center
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

    dest_x = int(game_logic.WIDTH / 2) - draw_size // 2
    dest_y = int(game_logic.HEIGHT / 2) - draw_size // 2
    screen.surface.blit(scaled, (dest_x, dest_y))
    
    # Draw solar flare flash effect if active
    _draw_solar_flare_flash(screen, game_logic)
    
    # Update UI panels with latest data
    if message_log:
        update_ui_panels(game_logic, ollama_model, message_log)


def _draw_solar_flare_flash(screen: Any, game_logic: Any) -> None:
    """Draw bright yellow flash overlay when solar flare hits (10 flashes in 2 seconds)."""
    if not game_logic.solar_flare_animation_active:
        return
    
    elapsed = time.time() - game_logic.solar_flare_animation_start_time
    
    # Animation lasts 2 seconds
    if elapsed >= 2.0:
        game_logic.solar_flare_animation_active = False
        return
    
    # 10 flashes in 2 seconds = flash every 0.2 seconds
    # Each flash cycle: 0.1s on, 0.1s off
    cycle_position = (elapsed % 0.2) / 0.2  # 0.0 to 1.0 within each 0.2s cycle
    
    # Show flash during first half of each cycle (0.0 to 0.5)
    if cycle_position < 0.5:
        flash_surface = pygame.Surface(screen.surface.get_size())
        flash_surface.set_alpha(200)  # Semi-transparent
        flash_surface.fill((255, 255, 0))  # Bright yellow
        screen.surface.blit(flash_surface, (0, 0))


def get_ui_manager() -> pygame_gui.UIManager | None:
    """Get the pygame_gui manager for event processing."""
    return _ui_manager
