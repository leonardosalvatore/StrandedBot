import importlib.resources
import time
import html
from typing import Any

import pygame
import pygame_gui
from pygame_gui import UI_BUTTON_PRESSED, UI_BUTTON_START_PRESS, UI_BUTTON_ON_HOVERED
from pygame_gui.elements import UIButton, UILabel, UIWindow, UITextBox, UITextEntryBox
from pygame_gui.elements.ui_selection_list import UISelectionList

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
_stats_text: UITextBox | None = None
_log_text: UITextBox | None = None
_speech_text: UITextBox | None = None
_prompt_text: UITextBox | None = None
_last_log_html: str | None = None
_last_speech_html: str | None = None
_last_stats_html: str | None = None
_last_prompt_html: str | None = None

_start_window: UIWindow | None = None
_start_default_button: UIButton | None = None
_start_custom_button: UIButton | None = None
_interactive_mode_checkbox: UIButton | None = None
_start_model_entry: UITextEntryBox | None = None
_interactive_mode_enabled = True
_custom_prompt_window: UIWindow | None = None
_custom_prompt_entry: UITextEntryBox | None = None
_custom_prompt_confirm_button: UIButton | None = None
_custom_prompt_cancel_button: UIButton | None = None
_user_reply_window: UIWindow | None = None
_user_reply_entry: UITextEntryBox | None = None
_user_reply_send_button: UIButton | None = None


def initialize_ui(screen_size: tuple[int, int], message_log: Any, default_model: str = "ministral-3:8b") -> pygame_gui.UIManager:
    """Initialize pygame_gui manager and create the 4 UI windows."""
    global _ui_manager, _stats_window, _log_window, _speech_window, _input_window
    global _stats_text, _log_text, _speech_text, _prompt_text
    
    _ui_manager = pygame_gui.UIManager(screen_size)
    
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
    ])
    
    # Create start menu FIRST so it's on top
    _create_start_menu(screen_size, default_model)
    
    # 1. Bot Stats Window (left-middle) - minimized by default
    _stats_window = UIWindow(
        rect=pygame.Rect((10, max(10, (screen_size[1] // 2) - 200)), (310, 400)),
        manager=_ui_manager,
        window_display_title="Bot Stats",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _stats_text = UITextBox(
        html_text="<font size=5>Initializing...</font>",
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_stats_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    
    # 2. Message Log Window (left side) - minimized by default
    _log_window = UIWindow(
        rect=pygame.Rect((10, 10), (400, 300)),
        manager=_ui_manager,
        window_display_title="Message Log",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _log_text = UITextBox(
        html_text="<font size=4>Message log started...</font>",
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_log_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    
    # 3. Bot Speech Window (top-right) - minimized by default
    _speech_window = UIWindow(
        rect=pygame.Rect((max(10, screen_size[0] - 630), 10), (620, 240)),
        manager=_ui_manager,
        window_display_title="Bot Speech",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _speech_text = UITextBox(
        html_text="<font size=5>Waiting for bot to speak...</font>",
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_speech_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    
    # 4. AI Prompt Window (top-left) - minimized by default
    _input_window = UIWindow(
        rect=pygame.Rect((10, 420), (410, 200)),
        manager=_ui_manager,
        window_display_title="Initial AI Prompt",
        resizable=True,
        visible=False,  # Hidden until game starts
    )
    _prompt_text = UITextBox(
        html_text="<font size=4><i>Prompt not selected yet.</i></font>",
        relative_rect=pygame.Rect((0, 0), (-10, -10)),
        manager=_ui_manager,
        container=_input_window,
        anchors={'left': 'left', 'right': 'right', 'top': 'top', 'bottom': 'bottom'}
    )
    
    message_log.start_capture()
    return _ui_manager


def _create_start_menu(screen_size: tuple[int, int], default_model: str) -> None:
    global _start_window, _start_default_button, _start_custom_button
    global _interactive_mode_checkbox, _start_model_entry, _interactive_mode_enabled
    if _ui_manager is None:
        print("[DEBUG] Cannot create start menu - UI manager is None")
        return

    win_w, win_h = 600, 300
    x = (screen_size[0] - win_w) // 2
    y = (screen_size[1] - win_h) // 2
    _start_window = UIWindow(
        rect=pygame.Rect((x, y), (win_w, win_h)),
        manager=_ui_manager,
        window_display_title="Start Game",
        resizable=False,
        draggable=False,  # Make it non-draggable so buttons work better
    )
    UILabel(
        relative_rect=pygame.Rect((15, 10), (400, 30)),
        text="Choose how to initialize the bot prompt:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_default_button = UIButton(
        relative_rect=pygame.Rect((20, 50), (390, 40)),
        text="Bot AI with default prompt",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_custom_button = UIButton(
        relative_rect=pygame.Rect((20, 100), (390, 40)),
        text="Bot with Custom prompt",
        manager=_ui_manager,
        container=_start_window,
    )
    UILabel(
        relative_rect=pygame.Rect((20, 150), (140, 30)),
        text="OLLAMA_MODEL:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_model_entry = UITextEntryBox(
        relative_rect=pygame.Rect((160, 150), (250, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_model_entry.set_text(default_model or "ministral-3:8b")
    _interactive_mode_checkbox = UIButton(
        relative_rect=pygame.Rect((20, 205), (390, 40)),
        text="Interactive mode, you can reply to Bot question.",
        manager=_ui_manager,
        container=_start_window,
    )
    _interactive_mode_enabled = True


def _open_custom_prompt_dialog() -> None:
    global _custom_prompt_window, _custom_prompt_entry
    global _custom_prompt_confirm_button, _custom_prompt_cancel_button
    if _ui_manager is None or _custom_prompt_window is not None:
        return

    _custom_prompt_window = UIWindow(
        rect=pygame.Rect((140, 120), (920, 520)),
        manager=_ui_manager,
        window_display_title="Custom Prompt",
        resizable=False,
    )
    UILabel(
        relative_rect=pygame.Rect((15, 10), (880, 25)),
        text="Enter the prompt text for the AI:",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_entry = UITextEntryBox(
        relative_rect=pygame.Rect((15, 40), (880, 370)),
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_confirm_button = UIButton(
        relative_rect=pygame.Rect((15, 430), (430, 50)),
        text="Start with custom prompt",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_cancel_button = UIButton(
        relative_rect=pygame.Rect((465, 430), (430, 50)),
        text="Cancel",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )


def open_custom_prompt_dialog(default_text: str) -> None:
    _open_custom_prompt_dialog()
    if _custom_prompt_entry is not None:
        _custom_prompt_entry.set_text(default_text)
    pygame.key.start_text_input()


def _close_custom_prompt_dialog() -> None:
    global _custom_prompt_window, _custom_prompt_entry
    global _custom_prompt_confirm_button, _custom_prompt_cancel_button
    if _custom_prompt_window is not None:
        _custom_prompt_window.kill()
    _custom_prompt_window = None
    _custom_prompt_entry = None
    _custom_prompt_confirm_button = None
    _custom_prompt_cancel_button = None
    pygame.key.stop_text_input()


def _close_start_menu() -> None:
    global _start_window, _start_default_button, _start_custom_button
    global _interactive_mode_checkbox, _start_model_entry
    if _start_window is not None:
        _start_window.kill()
    _start_window = None
    _start_default_button = None
    _start_custom_button = None
    _interactive_mode_checkbox = None
    _start_model_entry = None
    _close_custom_prompt_dialog()


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


def handle_startup_ui_event(event: pygame.event.Event) -> dict[str, Any] | None:
    global _interactive_mode_enabled
    # Use modern pygame_gui event.type instead of event.user_type
    if event.type == UI_BUTTON_PRESSED:

        if _start_default_button and event.ui_element == _start_default_button:
            model_name = "ministral-3:8b"
            if _start_model_entry is not None:
                model_name = _start_model_entry.get_text().strip() or "ministral-3:8b"
            _close_start_menu()
            return {"action": "start_default", "interactive_mode": _interactive_mode_enabled, "model": model_name}

        if _start_custom_button and event.ui_element == _start_custom_button:
            return {"action": "open_custom"}
        
        if _interactive_mode_checkbox and event.ui_element == _interactive_mode_checkbox:
            _interactive_mode_enabled = not _interactive_mode_enabled
            if _interactive_mode_enabled:
                _interactive_mode_checkbox.set_text("Interactive mode, you can reply to Bot question.")
            else:
                _interactive_mode_checkbox.set_text("Not interactive mode, Bot is all alone")
            return None

        if _custom_prompt_cancel_button and event.ui_element == _custom_prompt_cancel_button:
            _close_custom_prompt_dialog()
            return {"action": "cancel_custom"}

        if _custom_prompt_confirm_button and event.ui_element == _custom_prompt_confirm_button:
            prompt_text = ""
            if _custom_prompt_entry is not None:
                prompt_text = _custom_prompt_entry.get_text().strip()
            model_name = "ministral-3:8b"
            if _start_model_entry is not None:
                model_name = _start_model_entry.get_text().strip() or "ministral-3:8b"
            if not prompt_text:
                return {"action": "custom_prompt_empty"}
            _close_start_menu()
            return {"action": "start_custom", "prompt": prompt_text, "interactive_mode": _interactive_mode_enabled, "model": model_name}

    return None


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
    global _user_reply_window, _user_reply_entry, _user_reply_send_button
    if _user_reply_window is not None:
        _user_reply_window.kill()
    _user_reply_window = None
    _user_reply_entry = None
    _user_reply_send_button = None
    pygame.key.stop_text_input()


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
        inventory_text = html.escape(inventory_text)
        
        step_count = getattr(game_logic, "bot_step_count", 0)
        stats_html = (
            "<font size=4>"
            f"<b>Step:</b> {step_count}<br>"
            f"<b>Energy:</b> {game_logic.bot_energy}<br>"
            f"<b>Position:</b> ({gx}, {gy})<br>"
            f"<b>Tile:</b> {game_logic.tile_matrix[gx][gy].type}<br>"
            f"<b>Target:</b> ({tgx}, {tgy})<br>"
            f"<b>State:</b> {game_logic.bot_state}<br>"
            f"<b>Solar Flare in:</b> {game_logic.STEPS_TO_SOLAR_FLARE}<br>"
            f"<b>Habitats:</b> {habitats_total}<br>"
            f"<b>Inventory:</b> {inventory_text}<br>"
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
        log_html = "<font size=4>" + "<br>".join(messages[-50:]) + "</font>"
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
        speech_safe = html.escape(game_logic.bot_last_speech).replace(chr(10), '<br>')
        speech_html = f'<font size=5>🤖 {speech_safe}</font>'
        global _last_speech_html
        if speech_html != _last_speech_html:
            _speech_text.html_text = speech_html
            _speech_text.rebuild()
            _last_speech_html = speech_html

    # Update AI Prompt panel
    if _prompt_text:
        prompt_safe = html.escape(ai_prompt or "Prompt not selected yet.")
        prompt_html = f"<font size=4>{prompt_safe.replace(chr(10), '<br>')}</font>"
        global _last_prompt_html
        if prompt_html != _last_prompt_html:
            _prompt_text.html_text = prompt_html
            _prompt_text.rebuild()
            _last_prompt_html = prompt_html
    
    # Check if bot is waiting for user reply
    check_for_question_and_show_dialog()


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

    cam_world_x = game_logic.bot_x
    cam_world_y = game_logic.bot_y

    # Draw tiles
    with game_logic.tiles_lock:
        for tx in range(tile_x_start, tile_x_end):
            for ty in range(tile_y_start, tile_y_end):
                t = game_logic.tile_matrix[tx][ty]
                color = tuple(c // 2 for c in t.color) if t.fog else t.color
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
                        target_surface,
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
                target_surface,
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
    target_surface.blit(scaled, (dest_x, dest_y))
    
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

    _clamp_window_to_screen(_start_window, screen_size)
    _clamp_window_to_screen(_custom_prompt_window, screen_size)
    _clamp_window_to_screen(_user_reply_window, screen_size)
    _clamp_window_to_screen(_stats_window, screen_size)
    _clamp_window_to_screen(_log_window, screen_size)
    _clamp_window_to_screen(_speech_window, screen_size)
    _clamp_window_to_screen(_input_window, screen_size)
