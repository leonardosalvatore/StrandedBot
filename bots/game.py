import atexit

import pygame

from bots import game_logic
from bots.message_log import message_log
from bots.ollama_agent import get_ollama_settings, start_ollama_play, stop_ollama_model
from bots.rendering import draw_game, get_ui_manager, initialize_ui

WIDTH = game_logic.WIDTH
HEIGHT = game_logic.HEIGHT
TITLE = game_logic.TITLE

OLLAMA_MODEL, OLLAMA_PLAY = get_ollama_settings()

_ui_initialized = False


def _start_game() -> None:
    game_logic.initialize_world(use_fog=OLLAMA_PLAY)
    if OLLAMA_PLAY:
        start_ollama_play(game_logic, OLLAMA_MODEL)


def update(dt: float) -> None:
    global _ui_initialized
    
    # Initialize UI on first update (after pygame is ready)
    if not _ui_initialized:
        initialize_ui((WIDTH, HEIGHT), message_log)
        _ui_initialized = True
    
    # Process pygame events for pygame_gui
    ui_manager = get_ui_manager()
    if ui_manager:
        for event in pygame.event.get():
            ui_manager.process_events(event)
            # Re-post events for pgzero to handle
            pygame.event.post(event)
    
    # Update game logic
    game_logic.update(dt)
    
    # Update pygame_gui
    if ui_manager:
        ui_manager.update(dt)


def draw() -> None:
    draw_game(screen, Rect, game_logic, OLLAMA_MODEL, message_log)
    
    # Draw pygame_gui windows
    ui_manager = get_ui_manager()
    if ui_manager:
        ui_manager.draw_ui(screen.surface)


def run() -> None:
    import pgzrun

    pgzrun.go()


atexit.register(lambda: stop_ollama_model(OLLAMA_MODEL))
_start_game()
