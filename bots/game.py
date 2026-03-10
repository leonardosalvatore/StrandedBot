import atexit

import pygame

from bots import game_logic
from bots.message_log import message_log
from bots.ollama_agent import build_base_prompt, start_ollama_play, stop_ollama_model, submit_user_reply
from bots.rendering import (
    draw_game,
    get_ui_manager,
    handle_startup_ui_event,
    handle_user_reply_event,
        handle_user_reply_keyboard,
    initialize_ui,
    open_custom_prompt_dialog,
)

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


def _start_game() -> None:
    global _game_started, _world_initialized
    if not _world_initialized:
        game_logic.initialize_world(use_fog=OLLAMA_PLAY)
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
        start_ollama_play(game_logic, OLLAMA_MODEL, _active_prompt, GAME_INTERACTIVE_MODE)


def update(dt: float) -> None:
    global _ui_initialized, _active_prompt, _game_started, _world_initialized
    
    # Initialize UI on first update (after pygame is ready)
    if not _ui_initialized:
        initialize_ui((WIDTH, HEIGHT), message_log, OLLAMA_MODEL)
        _ui_initialized = True
    
    # Update pygame_gui (animations, hover states, etc.)
    ui_manager = get_ui_manager()
    if ui_manager:
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
    global _game_started, _world_initialized, _active_prompt, OLLAMA_MODEL
    
    ui_manager = get_ui_manager()
    if ui_manager:
        # Create pygame event and let pygame_gui process it  
        mouse_event = pygame.event.Event(pygame.MOUSEBUTTONUP, {'pos': pos, 'button': button})
        ui_manager.process_events(mouse_event)
        
        # After pygame_gui processes the click, check for button events it generated
        # We need to check the event queue for any UI events pygame_gui created
        for event in pygame.event.get():
            ui_manager.process_events(event)
            
            if not _game_started:
                startup_action = handle_startup_ui_event(event)
                if startup_action:
                    action = startup_action.get("action")
                    if action == "start_default":
                        interactive_mode = startup_action.get("interactive_mode", True)
                        selected_model = str(startup_action.get("model", OLLAMA_MODEL)).strip()
                        if selected_model:
                            OLLAMA_MODEL = selected_model
                        _start_game_with_prompt("", interactive_mode)
                    elif action == "start_custom":
                        interactive_mode = startup_action.get("interactive_mode", True)
                        selected_model = str(startup_action.get("model", OLLAMA_MODEL)).strip()
                        if selected_model:
                            OLLAMA_MODEL = selected_model
                        _start_game_with_prompt(str(startup_action.get("prompt", "")).strip(), interactive_mode)
                    elif action == "open_custom":
                        if not _world_initialized:
                            game_logic.initialize_world(use_fog=OLLAMA_PLAY)
                            _world_initialized = True
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
        return

    # Handle Enter/Shift-Enter in reply dialog
    if _game_started:
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

    # Only synthesize TEXTINPUT for printable characters.
    # Control chars like DEL/BS must be handled via KEYDOWN only.
    if safe_unicode:
        text_event = pygame.event.Event(
            pygame.TEXTINPUT,
            {
                "text": safe_unicode,
            },
        )
        ui_manager.process_events(text_event)


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
    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    if not isinstance(text, str) or not text or not text.isprintable():
        return

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
        ui_manager.draw_ui(screen.surface)


def run() -> None:
    import pgzrun

    pgzrun.go()


atexit.register(lambda: stop_ollama_model(OLLAMA_MODEL))
