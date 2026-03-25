import atexit
from collections import deque

import pygame
from pygame_gui import UI_BUTTON_PRESSED

from bots import game_logic
from bots.message_log import message_log
from bots.ollama_agent import build_base_prompt, start_ollama_play, stop_ollama_model, submit_user_reply
from bots.rendering import (
    bump_user_reply_deadline_if_waiting,
    draw_game,
    get_ui_manager,
    handle_user_reply_event,
    handle_user_reply_keyboard,
    initialize_ui,
    sync_ui_to_screen,
)
from bots.start_menu import handle_startup_ui_event, open_custom_prompt_dialog

WIDTH = game_logic.WIDTH
HEIGHT = game_logic.HEIGHT
TITLE = game_logic.TITLE

OLLAMA_MODEL = game_logic.OLLAMA_MODEL
OLLAMA_PLAY = game_logic.OLLAMA_PLAY

GAME_INTERACTIVE_MODE = True  # Default to true
_ui_initialized = False
_game_started = False
_active_prompt = ""
_world_initialized = False
_event_watcher_installed = False
_rocks_to_generate = game_logic.STARTING_WORLD_ROCKS_TARGET

# KEYDOWN often carries unicode for pgzero; we synthesize TEXTINPUT for pygame_gui. When
# pygame also delivers on_text_input for the same character, skip the duplicate (avoids crashes
# on select-all + type) while still allowing IME/paste-only TEXTINPUT when KEYDOWN had no unicode.
_pending_gui_textinput_skip: deque[str] = deque(maxlen=64)


def _parse_rocks_amount(value: object, default: int | None = None) -> int:
    d = game_logic.STARTING_WORLD_ROCKS_TARGET if default is None else default
    try:
        return max(1, int(value))
    except (TypeError, ValueError):
        return d


def _start_game() -> None:
    global _game_started, _world_initialized, _rocks_to_generate
    # Always generate the world when leaving the menu so settings (e.g. initial town size)
    # match the current form and we never reuse a map built before apply_* ran.
    game_logic.initialize_world(use_fog=OLLAMA_PLAY, rocks_target=_rocks_to_generate)
    _world_initialized = True
    _game_started = True
    # Show the game UI windows now that game has started
    from bots.rendering import show_game_windows
    show_game_windows()


def _start_game_with_prompt(prompt_text: str, interactive_mode: bool = True) -> None:
    global _active_prompt, GAME_INTERACTIVE_MODE, OLLAMA_MODEL
    GAME_INTERACTIVE_MODE = interactive_mode
    _active_prompt = prompt_text or build_base_prompt(game_logic)
    _start_game()
    if not prompt_text:
        _active_prompt = build_base_prompt(game_logic)
    if OLLAMA_PLAY:
        worker = start_ollama_play(game_logic, OLLAMA_MODEL, _active_prompt, GAME_INTERACTIVE_MODE)
        if worker is None:
            game_logic.bot_last_speech = "Ollama is not running. See: https://docs.ollama.com/linux"


def update(dt: float) -> None:
    global _ui_initialized, _active_prompt, _game_started, _world_initialized
    
    # Initialize UI on first update (after pygame is ready)
    if not _ui_initialized:
        initialize_ui((WIDTH, HEIGHT), message_log, OLLAMA_MODEL)
        _ui_initialized = True
    
    # Update pygame_gui (animations, hover states, etc.)
    ui_manager = get_ui_manager()
    if ui_manager:
        display_surface = pygame.display.get_surface()
        if display_surface is not None:
            sync_ui_to_screen(display_surface.get_size())
        ui_manager.update(dt)

    # Update game logic only after start choice is made
    if _game_started:
        game_logic.update(dt)


# pgzero event hooks - these are called automatically by pgzero's event loop
def on_mouse_down(pos, button):
    """Called by pgzero when mouse is clicked."""
    global _game_started, _world_initialized, _active_prompt
    
    ui_manager = get_ui_manager()
    if ui_manager:
        # Create pygame event and let pygame_gui process it
        mouse_event = pygame.event.Event(pygame.MOUSEBUTTONDOWN, {'pos': pos, 'button': button})
        ui_manager.process_events(mouse_event)


def on_mouse_up(pos, button):
    """Called by pgzero when mouse is released."""
    global _game_started, _world_initialized, _active_prompt, OLLAMA_MODEL, _rocks_to_generate
    
    ui_manager = get_ui_manager()
    if ui_manager:
        # Create pygame event and let pygame_gui process it  
        mouse_event = pygame.event.Event(pygame.MOUSEBUTTONUP, {'pos': pos, 'button': button})
        ui_manager.process_events(mouse_event)

        # Only dequeue pygame_gui button events so we do not drain the whole queue
        # (other events must remain for pgzero / pygame).
        for event in pygame.event.get(UI_BUTTON_PRESSED):
            ui_manager.process_events(event)
            
            if not _game_started:
                startup_action = handle_startup_ui_event(event)
                if startup_action:
                    action = startup_action.get("action")
                    if action == "start_default":
                        interactive_mode = startup_action.get("interactive_mode", True)
                        selected_model = str(startup_action.get("model", OLLAMA_MODEL)).strip()
                        _rocks_to_generate = _parse_rocks_amount(
                            startup_action.get("rocks_amount", _rocks_to_generate),
                            _rocks_to_generate,
                        )
                        if selected_model:
                            OLLAMA_MODEL = selected_model
                        game_logic.apply_initial_town_size(
                            int(
                                startup_action.get(
                                    "initial_town_size", game_logic.STARTING_INITIAL_TOWN_SIZE
                                )
                            )
                        )
                        game_logic.bot_energy = max(1, int(startup_action.get("energy", game_logic.STARTING_BOT_ENERGY)))
                        inv_rocks = max(0, int(startup_action.get("inventory_rocks", 0)))
                        game_logic.bot_inventory = [{"type": "rock"}] * inv_rocks
                        game_logic.apply_solar_flare_interval_hours(
                            int(startup_action.get("hours_solar_flare_every", game_logic.STARTING_HOURS_SOLAR_FLARE_EVERY))
                        )
                        game_logic.apply_ant_progression(
                            int(startup_action.get("ant_progression", game_logic.STARTING_ANT_PROGRESSION))
                        )
                        game_logic.apply_spawn_ant_after_hour(
                            int(
                                startup_action.get(
                                    "spawn_ant_after_hour", game_logic.STARTING_SPAWN_ANT_AFTER_HOUR
                                )
                            )
                        )
                        game_logic.apply_ant_hits_to_kill(
                            int(startup_action.get("ant_hits_to_kill", game_logic.STARTING_ANT_HITS_TO_KILL))
                        )
                        game_logic.apply_turret_bullet_rate(
                            float(startup_action.get("turret_bullet_rate", game_logic.STARTING_TURRET_BULLET_RATE))
                        )
                        _start_game_with_prompt("", interactive_mode)
                    elif action == "start_custom":
                        interactive_mode = startup_action.get("interactive_mode", True)
                        selected_model = str(startup_action.get("model", OLLAMA_MODEL)).strip()
                        _rocks_to_generate = _parse_rocks_amount(
                            startup_action.get("rocks_amount", _rocks_to_generate),
                            _rocks_to_generate,
                        )
                        if selected_model:
                            OLLAMA_MODEL = selected_model
                        game_logic.apply_initial_town_size(
                            int(
                                startup_action.get(
                                    "initial_town_size", game_logic.STARTING_INITIAL_TOWN_SIZE
                                )
                            )
                        )
                        game_logic.bot_energy = max(1, int(startup_action.get("energy", game_logic.STARTING_BOT_ENERGY)))
                        inv_rocks = max(0, int(startup_action.get("inventory_rocks", 0)))
                        game_logic.bot_inventory = [{"type": "rock"}] * inv_rocks
                        game_logic.apply_solar_flare_interval_hours(
                            int(startup_action.get("hours_solar_flare_every", game_logic.STARTING_HOURS_SOLAR_FLARE_EVERY))
                        )
                        game_logic.apply_ant_progression(
                            int(startup_action.get("ant_progression", game_logic.STARTING_ANT_PROGRESSION))
                        )
                        game_logic.apply_spawn_ant_after_hour(
                            int(
                                startup_action.get(
                                    "spawn_ant_after_hour", game_logic.STARTING_SPAWN_ANT_AFTER_HOUR
                                )
                            )
                        )
                        game_logic.apply_ant_hits_to_kill(
                            int(startup_action.get("ant_hits_to_kill", game_logic.STARTING_ANT_HITS_TO_KILL))
                        )
                        game_logic.apply_turret_bullet_rate(
                            float(startup_action.get("turret_bullet_rate", game_logic.STARTING_TURRET_BULLET_RATE))
                        )
                        _start_game_with_prompt(str(startup_action.get("prompt", "")).strip(), interactive_mode)
                    elif action == "open_custom":
                        _rocks_to_generate = _parse_rocks_amount(
                            startup_action.get("rocks_amount", _rocks_to_generate),
                            _rocks_to_generate,
                        )
                        open_custom_prompt_dialog(build_base_prompt(game_logic))
                    elif action == "custom_prompt_empty":
                        game_logic.bot_last_speech = "Custom prompt is empty. Please enter text first."
            else:
                # Game is running - check for user reply events
                reply_action = handle_user_reply_event(event)
                if reply_action and reply_action.get("action") == "send_reply":
                    reply_text = reply_action.get("reply", "")
                    if reply_text:
                        submit_user_reply(reply_text)


def on_mouse_wheel(x=0, y=0):
    """Forward mouse wheel to pygame_gui so text boxes can scroll."""
    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    wheel_event = pygame.event.Event(
        pygame.MOUSEWHEEL,
        {
            "x": x,
            "y": y,
            "flipped": False,
            "touch": False,
        },
    )
    ui_manager.process_events(wheel_event)


def on_key_down(key, mod, unicode=""):
    """Forward key presses to pygame_gui for text editing."""
    # F11 to toggle fullscreen
    if key == pygame.K_F11:
        pygame.display.toggle_fullscreen()
        ui_manager = get_ui_manager()
        if ui_manager:
            display_surface = pygame.display.get_surface()
            if display_surface is not None:
                sync_ui_to_screen(display_surface.get_size())
        return

    if _game_started:
        bump_user_reply_deadline_if_waiting()
        key_event = pygame.event.Event(
            pygame.KEYDOWN,
            {"key": key, "mod": mod, "unicode": unicode}
        )
        reply_action = handle_user_reply_keyboard(key_event)
        if reply_action and reply_action.get("action") == "send_reply":
            reply_text = reply_action.get("reply", "")
            if reply_text:
                submit_user_reply(reply_text)
                return  # Don't pass the Enter key to UI if we handled it

    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    safe_unicode = unicode if isinstance(unicode, str) and unicode.isprintable() else ""

    key_event = pygame.event.Event(
        pygame.KEYDOWN,
        {
            "key": key,
            "mod": mod,
            "unicode": safe_unicode,
        },
    )
    ui_manager.process_events(key_event)

    if safe_unicode:
        text_event = pygame.event.Event(
            pygame.TEXTINPUT,
            {"text": safe_unicode},
        )
        ui_manager.process_events(text_event)
        _pending_gui_textinput_skip.append(safe_unicode)


def on_key_up(key, mod):
    """Forward key releases to pygame_gui for text editing."""
    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    key_event = pygame.event.Event(
        pygame.KEYUP,
        {
            "key": key,
            "mod": mod,
        },
    )
    ui_manager.process_events(key_event)


def on_text_input(text):
    """Forward text input to pygame_gui for text boxes."""
    if _game_started:
        bump_user_reply_deadline_if_waiting()

    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    if not isinstance(text, str) or not text or not text.isprintable():
        return

    global _pending_gui_textinput_skip
    if _pending_gui_textinput_skip:
        if text == _pending_gui_textinput_skip[0]:
            _pending_gui_textinput_skip.popleft()
            return
        _pending_gui_textinput_skip.clear()

    text_event = pygame.event.Event(
        pygame.TEXTINPUT,
        {
            "text": text,
        },
    )
    ui_manager.process_events(text_event)


def draw() -> None:
    draw_game(screen, Rect, game_logic, OLLAMA_MODEL, _active_prompt, message_log)
    
    # Draw pygame_gui windows
    ui_manager = get_ui_manager()
    if ui_manager:
        display_surface = pygame.display.get_surface() or screen.surface
        ui_manager.draw_ui(display_surface)


def run() -> None:
    import pgzrun

    pgzrun.go()


atexit.register(lambda: stop_ollama_model(OLLAMA_MODEL))
