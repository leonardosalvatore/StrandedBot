import atexit

from bots import game_logic
from bots.ollama_agent import get_ollama_settings, start_ollama_play, stop_ollama_model
from bots.rendering import draw_game

WIDTH = game_logic.WIDTH
HEIGHT = game_logic.HEIGHT
TITLE = game_logic.TITLE

OLLAMA_MODEL, OLLAMA_PLAY = get_ollama_settings()


def _start_game() -> None:
    game_logic.initialize_world()
    if OLLAMA_PLAY:
        start_ollama_play(game_logic, OLLAMA_MODEL)


def update(dt: float) -> None:
    game_logic.update(dt)


def draw() -> None:
    draw_game(screen, Rect, game_logic, OLLAMA_MODEL)


def run() -> None:
    import pgzrun

    pgzrun.go()


atexit.register(lambda: stop_ollama_model(OLLAMA_MODEL))
_start_game()
