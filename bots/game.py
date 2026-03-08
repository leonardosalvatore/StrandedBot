import atexit

import pygame

from bots import game_logic
from bots.message_log import message_log
from bots.ollama_agent import build_base_prompt, get_ollama_settings, start_ollama_play, stop_ollama_model
from bots.rendering import draw_game, get_ui_manager, handle_startup_ui_event, initialize_ui, open_custom_prompt_dialog

WIDTH = game_logic.WIDTH
HEIGHT = game_logic.HEIGHT
TITLE = game_logic.TITLE

OLLAMA_MODEL, OLLAMA_PLAY = get_ollama_settings()

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


def _start_game_with_prompt(prompt_text: str) -> None:
    global _active_prompt
    _active_prompt = prompt_text or build_base_prompt(game_logic)
    _start_game()
    if not prompt_text:
        _active_prompt = build_base_prompt(game_logic)
    if OLLAMA_PLAY:
        start_ollama_play(game_logic, OLLAMA_MODEL, _active_prompt)


def update(dt: float) -> None:
    global _ui_initialized, _active_prompt, _game_started, _world_initialized
    
    # Initialize UI on first update (after pygame is ready)
    if not _ui_initialized:
        initialize_ui((WIDTH, HEIGHT), message_log)
        _ui_initialized = True
        print("[DEBUG] UI initialized, windows should be visible and interactive")
    
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
    global _game_started, _world_initialized, _active_prompt
    
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
                    print(f"[DEBUG] Startup action: {startup_action}")
                    action = startup_action.get("action")
                    if action == "start_default":
                        _start_game_with_prompt("")
                    elif action == "start_custom":
                        _start_game_with_prompt(str(startup_action.get("prompt", "")).strip())
                    elif action == "open_custom":
                        if not _world_initialized:
                            game_logic.initialize_world(use_fog=OLLAMA_PLAY)
                            _world_initialized = True
                        open_custom_prompt_dialog(build_base_prompt(game_logic))
                    elif action == "custom_prompt_empty":
                        game_logic.bot_last_speech = "Custom prompt is empty. Please enter text first."


def on_key_down(key, mod, unicode=""):
    """Forward key presses to pygame_gui for text editing."""
    ui_manager = get_ui_manager()
    if not ui_manager:
        return

    if not unicode:
        key_name = pygame.key.name(key)
        if len(key_name) == 1:
            unicode = key_name
            if mod & pygame.KMOD_SHIFT:
                unicode = unicode.upper()

    key_event = pygame.event.Event(
        pygame.KEYDOWN,
        {
            "key": key,
            "mod": mod,
            "unicode": unicode,
        },
    )
    ui_manager.process_events(key_event)

    if unicode:
        text_event = pygame.event.Event(
            pygame.TEXTINPUT,
            {
                "text": unicode,
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
