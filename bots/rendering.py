import importlib.resources
import math
import time
from typing import Any

import pygame
import pygame_gui
from pygame_gui import UI_BUTTON_PRESSED, UI_BUTTON_START_PRESS, UI_BUTTON_ON_HOVERED
from pygame_gui.elements import UIButton, UILabel, UIWindow, UITextEntryBox
from pygame_gui.elements.ui_selection_list import UISelectionList

from bots import game_logic
from bots import start_menu

try:
    from pygame_gui.core.ui_element import UIElement
    from pygame_gui.elements import UIPanel
except ImportError:
    pass


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
_stats_text: UITextEntryBox | None = None
_log_text: UITextEntryBox | None = None
_speech_text: UITextEntryBox | None = None
_prompt_text: UITextEntryBox | None = None
_last_log_text: str | None = None
_last_speech_text: str | None = None
_last_stats_text: str | None = None
_last_prompt_text: str | None = None

_user_reply_window: UIWindow | None = None
_user_reply_entry: UITextEntryBox | None = None
_user_reply_send_button: UIButton | None = None
_last_user_reply_window_title: str | None = None


def _darken_color(color: tuple[int, int, int], factor: float = 0.82) -> tuple[int, int, int]:
    return tuple(max(0, min(255, int(c * factor))) for c in color)


def _draw_rock_triangle(surface: pygame.Surface, tile_rect: pygame.Rect, color: tuple[int, int, int]) -> None:
    """Draw a tiny filled triangle marker for rock tiles."""
    if tile_rect.width < 6 or tile_rect.height < 6:
        return

    mark_color = _darken_color(color, 0.62)
    cx = tile_rect.centerx
    top_y = tile_rect.top + 2
    base_y = min(tile_rect.bottom - 2, top_y + max(2, tile_rect.height // 3))
    half_base = max(1, tile_rect.width // 5)
    points = [(cx, top_y), (cx - half_base, base_y), (cx + half_base, base_y)]
    pygame.draw.polygon(surface, mark_color, points)


def _draw_sand_dots(
    surface: pygame.Surface,
    tile_rect: pygame.Rect,
    color: tuple[int, int, int],
    tx: int,
    ty: int,
    dot_count: int = 3,
    seed_offset: int = 0,
) -> None:
    """Draw tiny deterministic speckle dots for granular/liquid tiles."""
    if tile_rect.width < 6 or tile_rect.height < 6:
        return

    mark_color = _darken_color(color, 0.7)
    seed = ((tx * 73856093) ^ (ty * 19349663) ^ seed_offset) & 0xFFFFFFFF
    inner_left = tile_rect.left + 2
    inner_top = tile_rect.top + 2
    inner_w = max(1, tile_rect.width - 4)
    inner_h = max(1, tile_rect.height - 4)

    for i in range(max(1, dot_count)):
        x = inner_left + ((seed >> (i * 5)) % inner_w)
        y = inner_top + ((seed >> (i * 7 + 2)) % inner_h)
        pygame.draw.line(surface, mark_color, (x, y), (x, y), 1)


def _draw_habitat_circle(
    surface: pygame.Surface,
    tile_rect: pygame.Rect,
    color: tuple[int, int, int],
    *,
    fill_blue: bool = False,
) -> None:
    """Draw a small center circle; habitats get a blue-filled interior."""
    if tile_rect.width < 6 or tile_rect.height < 6:
        return

    radius = max(1, min(tile_rect.width, tile_rect.height) // 5)
    if fill_blue:
        pygame.draw.circle(surface, (72, 130, 220), tile_rect.center, radius)
    mark_color = _darken_color(color, 0.6)
    pygame.draw.circle(surface, mark_color, tile_rect.center, radius, 1)


def _draw_battery_marker(surface: pygame.Surface, tile_rect: pygame.Rect, color: tuple[int, int, int]) -> None:
    """Battery shell fills the tile (small margin); lightning bolt fills the main body."""
    if tile_rect.width < 6 or tile_rect.height < 6:
        return

    battery_color = (205, 210, 215)
    bolt_color = (245, 248, 250)
    edge = _darken_color(battery_color, 0.78)

    pad = max(1, min(tile_rect.width, tile_rect.height) // 14)
    inner = tile_rect.inflate(-2 * pad, -2 * pad)

    tab_w = max(4, inner.width * 9 // 20)
    tab_h = max(2, inner.height // 6)
    terminal_rect = pygame.Rect(0, 0, tab_w, tab_h)
    terminal_rect.midtop = (inner.centerx, inner.top)

    body_rect = pygame.Rect(
        inner.left,
        inner.top + tab_h,
        inner.width,
        max(4, inner.height - tab_h),
    )
    brad = max(1, min(body_rect.width, body_rect.height) // 6)
    pygame.draw.rect(surface, battery_color, body_rect, border_radius=brad)
    pygame.draw.rect(surface, edge, body_rect, 1, border_radius=brad)
    pygame.draw.rect(surface, battery_color, terminal_rect)

    # Bolt uses almost the full body (minimal margin); same zig-zag topology as before, scaled up.
    m = max(1, min(body_rect.width, body_rect.height) // 28)
    bolt_left = body_rect.left + m
    bolt_right = body_rect.right - m
    bolt_top = body_rect.top + m
    bolt_bottom = body_rect.bottom - m
    bolt_mid_y = (bolt_top + bolt_bottom) // 2
    cx = body_rect.centerx
    bolt_points = [
        (bolt_right, bolt_top),
        (bolt_left, bolt_mid_y - max(1, m)),
        (cx + max(1, m * 2), bolt_mid_y - max(1, m)),
        (bolt_left, bolt_bottom),
        (bolt_right - max(1, m * 2), bolt_mid_y),
        (cx, bolt_mid_y),
    ]
    pygame.draw.polygon(surface, bolt_color, bolt_points)


def _draw_solar_panel_grid(surface: pygame.Surface, tile_rect: pygame.Rect, color: tuple[int, int, int]) -> None:
    """Draw a tiny 9x9 panel grid inside a solar panel tile."""
    if tile_rect.width < 10 or tile_rect.height < 10:
        return

    grid_color = _darken_color(color, 0.5)
    inset = 2
    left = tile_rect.left + inset
    top = tile_rect.top + inset
    width = max(2, tile_rect.width - inset * 2)
    height = max(2, tile_rect.height - inset * 2)

    pygame.draw.rect(surface, grid_color, pygame.Rect(left, top, width, height), 1)

    # 9x9 cells => 8 inner dividers each direction.
    for i in range(1, 9):
        x = left + int((i * width) / 9)
        y = top + int((i * height) / 9)
        pygame.draw.line(surface, grid_color, (x, top), (x, top + height - 1), 1)
        pygame.draw.line(surface, grid_color, (left, y), (left + width - 1, y), 1)


def _turret_is_operational_unlocked(gx: int, gy: int) -> bool:
    """Call only while holding game_logic.tiles_lock."""
    if game_logic.tile_matrix[gx][gy].type != "turret":
        return False
    has_solar = has_sink = False
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = gx + dx, gy + dy
        if not (0 <= nx < game_logic.GRID_WIDTH and 0 <= ny < game_logic.GRID_HEIGHT):
            continue
        typ = game_logic.tile_matrix[nx][ny].type
        if typ == "solar_panel":
            has_solar = True
        if typ in {"habitat", "battery"}:
            has_sink = True
    return has_solar and has_sink


def _turret_aim_angle_rad(
    gx: int,
    gy: int,
    *,
    under_tiles_lock: bool = False,
    ants_xy: list[tuple[float, float]] | None = None,
) -> float:
    """Yaw (radians) toward nearest in-range ant; idle east if none.

    When under_tiles_lock=True, pass ants_xy from a snapshot taken *before* acquiring tiles_lock
    (never acquire ants_lock while holding tiles_lock — that deadlocks with _spawn_ants).
    """
    ts = game_logic.TILE_SIZE
    tcx = gx * ts + ts / 2
    tcy = gy * ts + ts / 2
    range_px = game_logic.TURRET_RANGE_TILES * ts
    range_px2 = range_px * range_px
    if under_tiles_lock:
        operational = _turret_is_operational_unlocked(gx, gy)
    else:
        with game_logic.tiles_lock:
            operational = _turret_is_operational_unlocked(gx, gy)
    if not operational:
        return 0.0
    best_d2 = float("inf")
    best_dx = 0.0
    best_dy = 0.0
    if ants_xy is not None:
        for ax, ay in ants_xy:
            d2 = (ax - tcx) ** 2 + (ay - tcy) ** 2
            if d2 <= range_px2 and d2 < best_d2:
                best_d2 = d2
                best_dx = ax - tcx
                best_dy = ay - tcy
    else:
        with game_logic.ants_lock:
            for a in game_logic.ants:
                d2 = (a.x - tcx) ** 2 + (a.y - tcy) ** 2
                if d2 <= range_px2 and d2 < best_d2:
                    best_d2 = d2
                    best_dx = a.x - tcx
                    best_dy = a.y - tcy
    if best_d2 <= range_px2:
        return math.atan2(best_dy, best_dx)
    return 0.0


def _draw_turret_rect(
    surface: pygame.Surface,
    tile_rect: pygame.Rect,
    color: tuple[int, int, int],
    aim_angle_rad: float = 0.0,
) -> None:
    """Military-green base plus barrel pointing along aim_angle_rad (from +x)."""
    if tile_rect.width < 6 or tile_rect.height < 6:
        return
    inset = max(2, min(tile_rect.width, tile_rect.height) // 6)
    body = pygame.Rect(
        tile_rect.left + inset,
        tile_rect.top + inset,
        max(2, tile_rect.width - inset * 2),
        max(2, tile_rect.height - inset * 2),
    )
    pygame.draw.rect(surface, color, body, border_radius=1)
    pygame.draw.rect(surface, _darken_color(color, 0.62), body, 1, border_radius=1)

    cx, cy = float(tile_rect.centerx), float(tile_rect.centery)
    blen = min(tile_rect.width, tile_rect.height) * 0.48
    ex = cx + math.cos(aim_angle_rad) * blen
    ey = cy + math.sin(aim_angle_rad) * blen
    bw = max(2, min(tile_rect.width, tile_rect.height) // 5)
    barrel = _darken_color(color, 0.5)
    pygame.draw.line(surface, barrel, (cx, cy), (ex, ey), bw)
    pygame.draw.line(
        surface,
        _darken_color(color, 0.72),
        (cx, cy),
        (ex, ey),
        max(1, bw - 2),
    )


def _draw_turret_lasers(
    target_surface: pygame.Surface,
    lasers: list[Any],
    ant_by_id: dict[int, tuple[float, float]],
    cam_world_x: float,
    cam_world_y: float,
    scale: float,
    map_width: int,
    map_height: int,
) -> None:
    """White laser beams from turret muzzle to target ant (instant hit; VFX only)."""
    w_wide = max(2, int(round(scale * 0.55)))
    w_core = max(1, min(3, int(round(scale * 0.22))))
    underlay = (210, 210, 220)
    core = (255, 255, 255)
    for L in lasers:
        end = ant_by_id.get(L.target_ant_id)
        if end is None:
            continue
        ax, ay = end
        x0 = (L.muzzle_x - cam_world_x) * scale + map_width / 2
        y0 = (L.muzzle_y - cam_world_y) * scale + map_height / 2
        x1 = (ax - cam_world_x) * scale + map_width / 2
        y1 = (ay - cam_world_y) * scale + map_height / 2
        pygame.draw.line(target_surface, underlay, (x0, y0), (x1, y1), w_wide)
        pygame.draw.line(target_surface, core, (x0, y0), (x1, y1), w_core)


def initialize_ui(
    screen_size: tuple[int, int], message_log: Any, default_model: str | None = None
) -> pygame_gui.UIManager:
    """Initialize pygame_gui manager and create the 4 UI windows."""
    global _ui_manager, _stats_window, _log_window, _speech_window, _input_window
    global _stats_text, _log_text, _speech_text, _prompt_text
    
    _ui_manager = pygame_gui.UIManager(screen_size)
    start_menu.set_ui_manager(_ui_manager)

    # Preload fonts to avoid warnings
    _ui_manager.preload_fonts([
        {'name': 'noto_sans', 'point_size': 12, 'style': 'regular', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 12, 'style': 'italic', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 12, 'style': 'bold', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 14, 'style': 'regular', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 14, 'style': 'italic', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 14, 'style': 'bold', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 16, 'style': 'regular', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 16, 'style': 'italic', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 16, 'style': 'bold', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 18, 'style': 'regular', 'antialiased': '1'},
        {'name': 'noto_sans', 'point_size': 18, 'style': 'bold', 'antialiased': '1'},
        # Try to load an emoji-capable font for the bot icon if available on system.
        {'name': 'noto_color_emoji', 'point_size': 18, 'style': 'regular', 'antialiased': '1'},
    ])
    
    # Create start menu FIRST so it's on top
    start_menu.create_start_menu(screen_size, default_model or game_logic.OLLAMA_MODEL)
    
    # 1. Bot Stats Window (left-middle) - minimized by default
    stats_y = max(10, (screen_size[1] // 2) - 200)
    _stats_window = UIWindow(
        rect=pygame.Rect((10, stats_y), (310, 400)),
        manager=_ui_manager,
        window_display_title="Bot Stats",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _stats_text = UITextEntryBox(
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_stats_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    _stats_text.set_text("Initializing...")
    
    # 2. Message Log Window (left side) - minimized by default
    _log_window = UIWindow(
        rect=pygame.Rect((10, 10), (400, 300)),
        manager=_ui_manager,
        window_display_title="Message Log",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _log_text = UITextEntryBox(
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_log_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    _log_text.set_text("Message log started...")
    
    # 3. Bot Speech Window (top-right) - minimized by default
    _speech_window = UIWindow(
        rect=pygame.Rect((max(10, screen_size[0] - 630), 10), (620, 720)),
        manager=_ui_manager,
        window_display_title="Bot Speech",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _speech_text = UITextEntryBox(
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_speech_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    _speech_text.set_text("Waiting for bot to speak...")
    
    # 4. AI Prompt Window (below Bot Stats) - minimized by default
    _input_window = UIWindow(
        rect=pygame.Rect((10, stats_y + 410), (410, 200)),
        manager=_ui_manager,
        window_display_title="Initial AI Prompt",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _prompt_text = UITextEntryBox(
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_input_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    _prompt_text.set_text("Prompt not selected yet.")
    
    message_log.start_capture()
    return _ui_manager


def show_game_windows() -> None:
    """Show the game UI windows after game starts."""
    global _stats_window, _log_window, _speech_window, _input_window
    if _stats_window:
        _stats_window.show()
    if _log_window:
        _log_window.show()
    if _speech_window:
        _speech_window.show()
    if _input_window:
        _input_window.show()


def open_user_reply_dialog() -> None:
    """Open the user reply dialog window."""
    global _user_reply_window, _user_reply_entry, _user_reply_send_button
    if _ui_manager is None or _user_reply_window is not None:
        return
    
    win_w, win_h = 620, 220
    screen_size = _ui_manager.window_resolution
    x = screen_size[0] - win_w - 10
    y = screen_size[1] - win_h - 10
    
    _user_reply_window = UIWindow(
        rect=pygame.Rect((x, y), (win_w, win_h)),
        manager=_ui_manager,
        window_display_title="Bot is waiting for your reply",
        resizable=False,
    )
    UILabel(
        relative_rect=pygame.Rect((15, 10), (580, 25)),
        text="The bot asked you a question. Enter your reply:",
        manager=_ui_manager,
        container=_user_reply_window,
    )
    _user_reply_entry = UITextEntryBox(
        relative_rect=pygame.Rect((15, 45), (580, 100)),
        manager=_ui_manager,
        container=_user_reply_window,
    )
    _user_reply_send_button = UIButton(
           relative_rect=pygame.Rect((425, 155), (180, 40)),
        text="Send Reply",
        manager=_ui_manager,
        container=_user_reply_window,
    )
    pygame.key.start_text_input()


def close_user_reply_dialog() -> None:
    """Close the user reply dialog window."""
    global _user_reply_window, _user_reply_entry, _user_reply_send_button, _last_user_reply_window_title
    if _user_reply_window is not None:
        _user_reply_window.kill()
    _user_reply_window = None
    _user_reply_entry = None
    _user_reply_send_button = None
    _last_user_reply_window_title = None
    pygame.key.stop_text_input()


def bump_user_reply_deadline_if_waiting() -> None:
    """Call on keyboard/text input so idle auto-reply timer restarts while the reply window is open."""
    from bots import ollama_agent

    if _user_reply_window is None:
        return
    if ollama_agent.is_waiting_for_reply():
        ollama_agent.bump_user_reply_deadline()


def _sync_user_reply_window_title() -> None:
    """Show countdown in the reply window title bar; typing resets the underlying deadline."""
    global _last_user_reply_window_title
    from bots import ollama_agent

    if _user_reply_window is None:
        _last_user_reply_window_title = None
        return
    remaining = ollama_agent.get_user_reply_seconds_remaining()
    if remaining is None:
        return
    secs = max(0, int(math.ceil(remaining)))
    max_idle = int(ollama_agent.USER_REPLY_TIMEOUT_SEC)
    title = (
        f"Bot is waiting — {secs}s / {max_idle}s idle (typing resets)"
    )
    if title != _last_user_reply_window_title:
        _user_reply_window.set_display_title(title)
        _last_user_reply_window_title = title


def handle_user_reply_event(event: pygame.event.Event) -> dict[str, Any] | None:
    """Handle user reply dialog events."""
    if event.type == UI_BUTTON_PRESSED:
        if _user_reply_send_button and event.ui_element == _user_reply_send_button:
            reply_text = ""
            if _user_reply_entry is not None:
                reply_text = _user_reply_entry.get_text().strip()
            if reply_text:
                close_user_reply_dialog()
                return {"action": "send_reply", "reply": reply_text}
    return None


def handle_user_reply_keyboard(event: pygame.event.Event) -> dict[str, Any] | None:
    """Handle keyboard events for the reply dialog. Enter sends, Shift-Enter creates new line."""
    if _user_reply_window is None or _user_reply_entry is None:
        return None

    if event.type == pygame.KEYDOWN:
        # Check if Enter is pressed without Shift modifier
        if event.key == pygame.K_RETURN or event.key == pygame.K_KP_ENTER:
            # If Shift is held, allow normal newline behavior
            if event.mod & pygame.KMOD_SHIFT:
                return None
            # Otherwise, send the reply
            reply_text = _user_reply_entry.get_text().strip()
            if reply_text:
                close_user_reply_dialog()
                return {"action": "send_reply", "reply": reply_text}
    return None


def check_for_question_and_show_dialog() -> None:
    """Check if bot is waiting for user reply and show dialog if needed."""
    from bots import ollama_agent

    if ollama_agent.consume_user_reply_dialog_close_request():
        close_user_reply_dialog()

    if ollama_agent.is_waiting_for_reply() and _user_reply_window is None:
        open_user_reply_dialog()


def update_ui_panels(game_logic: Any, ollama_model: str, message_log: Any, ai_prompt: str) -> None:
    """Update the content of all UI panels."""
    if not _ui_manager:
        return
        
    gx, gy = game_logic._bot_grid_pos()
    tgx = max(0, min(game_logic.GRID_WIDTH - 1, int(game_logic.bot_target_x) // game_logic.TILE_SIZE))
    tgy = max(0, min(game_logic.GRID_HEIGHT - 1, int(game_logic.bot_target_y) // game_logic.TILE_SIZE))
    
    # Update Bot Stats
    if _stats_text:
        habitats_total = len(game_logic.habitat_damage)

        inventory_items = getattr(game_logic, "bot_inventory", []) or []
        if inventory_items:
            counts: dict[str, int] = {}
            for item in inventory_items:
                item_type = str(item.get("type", "unknown")) if isinstance(item, dict) else "unknown"
                counts[item_type] = counts.get(item_type, 0) + 1
            inventory_text = ", ".join(f"{item_type} x{count}" for item_type, count in sorted(counts.items()))
        else:
            inventory_text = "(empty)"
        
        hour_count = getattr(game_logic, "bot_hour_count", 0)
        stats_text = (
            f"hour: {hour_count}\n"
            f"Energy: {game_logic.bot_energy}\n"
            f"Position: ({gx}, {gy})\n"
            f"Tile: {game_logic.tile_matrix[gx][gy].type}\n"
            f"Target: ({tgx}, {tgy})\n"
            f"State: {game_logic.bot_state}\n"
            f"Solar Flare in: {game_logic.HOURS_TO_SOLAR_FLARE}\n"
            f"Habitats: {habitats_total}\n"
            f"Inventory: {inventory_text}\n"
            f"Model: {ollama_model}"
        )
        global _last_stats_text
        if stats_text != _last_stats_text:
            _stats_text.set_text(stats_text)
            _last_stats_text = stats_text
    
    # Update Message Log (last 50 messages)
    if _log_text:
        messages = message_log.get_messages(50)
        log_text = "\n".join(messages[-50:])
        global _last_log_text
        if log_text != _last_log_text:
            should_follow = False
            if hasattr(_log_text, "scroll_bar") and _log_text.scroll_bar:
                # Only auto-follow if user is already near the bottom.
                should_follow = _log_text.scroll_bar.scroll_position >= (
                    _log_text.scroll_bar.bottom_limit - 5
                )
            _log_text.set_text(log_text)
            _last_log_text = log_text
            if should_follow and _log_text.scroll_bar:
                _log_text.scroll_bar.scroll_position = _log_text.scroll_bar.bottom_limit
    
    # Update Bot Speech
    if _speech_text and game_logic.bot_last_speech:
        speech_text = f"[BOT] {game_logic.bot_last_speech}"
        global _last_speech_text
        if speech_text != _last_speech_text:
            _speech_text.set_text(speech_text)
            _last_speech_text = speech_text

    # Update AI Prompt panel
    if _prompt_text:
        prompt_text = ai_prompt or "Prompt not selected yet."
        global _last_prompt_text
        if prompt_text != _last_prompt_text:
            _prompt_text.set_text(prompt_text)
            _last_prompt_text = prompt_text
    
    # Check if bot is waiting for user reply
    check_for_question_and_show_dialog()
    _sync_user_reply_window_title()


def draw_game(
    screen: Any,
    Rect: Any,
    game_logic: Any,
    ollama_model: str,
    ai_prompt: str,
    message_log: Any = None,
) -> None:
    """Draw the game map and bot sprite (UI windows are drawn separately by pygame_gui)."""
    global _SPRITE_SHEET
    target_surface = pygame.display.get_surface() or screen.surface
    
    # Clear only the map area (not UI)
    target_surface.fill((0, 0, 0))
    
    gx, gy = game_logic._bot_grid_pos()
    cam_tile_x = gx
    cam_tile_y = gy

    half_w = game_logic.VIEWPORT_TILES_W // 2
    half_h = game_logic.VIEWPORT_TILES_H // 2
    tile_x_start = max(0, cam_tile_x - half_w)
    tile_x_end = min(game_logic.GRID_WIDTH, cam_tile_x + half_w + 1)
    tile_y_start = max(0, cam_tile_y - half_h)
    tile_y_end = min(game_logic.GRID_HEIGHT, cam_tile_y + half_h + 1)

    # Bot world coordinates are tile-anchored; add half-tile so centered sprite sits in tile center.
    cam_world_x = game_logic.bot_x + (game_logic.TILE_SIZE / 2)
    cam_world_y = game_logic.bot_y + (game_logic.TILE_SIZE / 2)

    # Ant positions for turret aim: snapshot before tiles_lock (avoid deadlock with _spawn_ants).
    with game_logic.ants_lock:
        ants_xy_for_turrets = [(a.x, a.y) for a in game_logic.ants]

    # Draw tiles
    with game_logic.tiles_lock:
        for tx in range(tile_x_start, tile_x_end):
            for ty in range(tile_y_start, tile_y_end):
                t = game_logic.tile_matrix[tx][ty]
                color = tuple(c // 3 for c in t.color) if t.fog else t.color
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
                    tile_rect = pygame.Rect(
                        int(screen_x),
                        int(screen_y),
                        game_logic.DRAW_TILE_SIZE,
                        game_logic.DRAW_TILE_SIZE,
                    )
                    pygame.draw.rect(
                        target_surface,
                        color,
                        tile_rect,
                    )

                    # Add a subtle inner 1px corner accent to reduce flat-looking tiles.
                    if tile_rect.width >= 4 and tile_rect.height >= 4:
                        accent = _darken_color(color)
                        left_x = tile_rect.left + 1
                        bottom_y = tile_rect.bottom - 2
                        horizontal_end_x = min(tile_rect.right - 2, left_x + max(1, tile_rect.width // 0.8))
                        vertical_top_y = max(tile_rect.top + 1, bottom_y - max(1, tile_rect.height // 0.8))

                        pygame.draw.line(
                            target_surface,
                            accent,
                            (left_x, bottom_y),
                            (horizontal_end_x, bottom_y),
                            1,
                        )
                        pygame.draw.line(
                            target_surface,
                            accent,
                            (left_x, vertical_top_y),
                            (left_x, bottom_y),
                            1,
                        )

                    if t.type == "rocks":
                        _draw_rock_triangle(target_surface, tile_rect, color)
                    elif t.type == "sand":
                        _draw_sand_dots(target_surface, tile_rect, color, tx, ty)
                    elif t.type == "gravel":
                        _draw_sand_dots(target_surface, tile_rect, color, tx, ty, dot_count=5, seed_offset=0x9E3779B9)
                    elif t.type == "water":
                        _draw_sand_dots(target_surface, tile_rect, color, tx, ty, dot_count=3, seed_offset=0x85EBCA77)
                    elif t.type == "habitat":
                        _draw_habitat_circle(target_surface, tile_rect, color, fill_blue=True)
                    elif t.type == "broken_habitat":
                        _draw_habitat_circle(target_surface, tile_rect, color, fill_blue=False)
                    elif t.type == "battery":
                        _draw_battery_marker(target_surface, tile_rect, color)
                    elif t.type == "solar_panel":
                        _draw_solar_panel_grid(target_surface, tile_rect, color)
                    elif t.type == "turret":
                        _draw_turret_rect(
                            target_surface,
                            tile_rect,
                            color,
                            _turret_aim_angle_rad(
                                tx,
                                ty,
                                under_tiles_lock=True,
                                ants_xy=ants_xy_for_turrets,
                            ),
                        )

    scale = game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE
    with game_logic.ants_lock:
        ants_copy = list(game_logic.ants)
        lasers_copy = list(game_logic.turret_lasers)
    ant_by_id = {a.id: (a.x, a.y) for a in ants_copy}
    ant_radius = max(1, int(max(4, game_logic.BOT_RADIUS // 2) * scale / 5))
    for ant in ants_copy:
        sx = ((ant.x - cam_world_x) * scale) + (game_logic.WIDTH / 2)
        sy = ((ant.y - cam_world_y) * scale) + (game_logic.HEIGHT / 2)
        if 0 <= sx < game_logic.WIDTH and 0 <= sy < game_logic.HEIGHT:
            pygame.draw.circle(target_surface, (220, 40, 40), (int(round(sx)), int(round(sy))), ant_radius)
            pygame.draw.circle(target_surface, (120, 20, 20), (int(round(sx)), int(round(sy))), ant_radius, 1)

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
                target_surface,
                (0, 120, 255),
                (game_logic.WIDTH // 2, game_logic.HEIGHT // 2),
                radius_scaled,
            )
            with game_logic.ants_lock:
                _ants = list(game_logic.ants)
                _lasers = list(game_logic.turret_lasers)
            _draw_turret_lasers(
                target_surface,
                _lasers,
                {a.id: (a.x, a.y) for a in _ants},
                cam_world_x,
                cam_world_y,
                scale,
                game_logic.WIDTH,
                game_logic.HEIGHT,
            )
            return

    # Draw bot sprite centered on the bot's current tile projection.
    col, row = _STATE_SPRITE_POS.get(game_logic.bot_state, (0, 0))
    src_rect = pygame.Rect(
        col * _SPRITE_SIZE,
        row * _SPRITE_SIZE,
        _SPRITE_SIZE,
        _SPRITE_SIZE,
    )
    sprite = _SPRITE_SHEET.subsurface(src_rect)

    draw_size = max(8, int(game_logic.BOT_RADIUS * 6 * (game_logic.DRAW_TILE_SIZE / game_logic.TILE_SIZE) / 4))
    scaled = pygame.transform.smoothscale(sprite, (draw_size, draw_size))

    bot_center_world_x = game_logic.bot_x + (game_logic.TILE_SIZE / 2)
    bot_center_world_y = game_logic.bot_y + (game_logic.TILE_SIZE / 2)
    bot_center_screen_x = ((bot_center_world_x - cam_world_x) * scale) + (game_logic.WIDTH / 2)
    bot_center_screen_y = ((bot_center_world_y - cam_world_y) * scale) + (game_logic.HEIGHT / 2)

    dest_rect = scaled.get_rect(
        center=(int(round(bot_center_screen_x)), int(round(bot_center_screen_y)))
    )
    target_surface.blit(scaled, dest_rect)

    _draw_turret_lasers(
        target_surface,
        lasers_copy,
        ant_by_id,
        cam_world_x,
        cam_world_y,
        scale,
        game_logic.WIDTH,
        game_logic.HEIGHT,
    )

    # Draw solar flare flash effect if active
    _draw_solar_flare_flash(target_surface, game_logic)
    
    # Update UI panels with latest data
    if message_log:
        update_ui_panels(game_logic, ollama_model, message_log, ai_prompt)


def _draw_solar_flare_flash(surface: pygame.Surface, game_logic: Any) -> None:
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
        flash_surface = pygame.Surface(surface.get_size())
        flash_surface.set_alpha(200)  # Semi-transparent
        flash_surface.fill((255, 255, 0))  # Bright yellow
        surface.blit(flash_surface, (0, 0))


def get_ui_manager() -> pygame_gui.UIManager | None:
    """Get the pygame_gui manager for event processing."""
    return _ui_manager


def _clamp_window_to_screen(window: UIWindow | None, screen_size: tuple[int, int]) -> None:
    if window is None:
        return

    rect = window.get_abs_rect()
    max_x = max(0, screen_size[0] - rect.width)
    max_y = max(0, screen_size[1] - rect.height)
    new_x = max(0, min(rect.x, max_x))
    new_y = max(0, min(rect.y, max_y))

    if new_x != rect.x or new_y != rect.y:
        window.set_position((new_x, new_y))


def sync_ui_to_screen(screen_size: tuple[int, int]) -> None:
    """Keep pygame_gui resolution and windows aligned with current display size."""
    if _ui_manager is None:
        return

    current_size = tuple(_ui_manager.window_resolution)
    if current_size != tuple(screen_size):
        _ui_manager.set_window_resolution(screen_size)

    start_menu.clamp_start_menu_windows(screen_size)
    _clamp_window_to_screen(_user_reply_window, screen_size)
    _clamp_window_to_screen(_stats_window, screen_size)
    _clamp_window_to_screen(_log_window, screen_size)
    _clamp_window_to_screen(_speech_window, screen_size)
    _clamp_window_to_screen(_input_window, screen_size)
