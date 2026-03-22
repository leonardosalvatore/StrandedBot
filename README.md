Local LLM (Ollama) plays a small tile survival game: a robot gathers rocks, builds habitats, and survives solar flares.

[Short video demo](https://youtu.be/esa-aX58YaI) · ![Screenshot](bots/resources/screenshot.jpg)

## Run

```bash
poetry install
poetry run bots
```

**LLM play:** `OLLAMA_PLAY=1 poetry run bots` — start screen lets you pick model, rock clusters, energy, starting inventory rocks, **hours between solar flares**, and interactive reply mode.

## Env

| Var | Default | Purpose |
|-----|---------|---------|
| `OLLAMA_PLAY` | `0` | `1` = autonomous LLM |
| `OLLAMA_MODEL` | see `game_logic.py` | Model if not overridden in UI |
| `BOTS_SIM_HOUR_WALL_SEC` | `2` | Game hours advance while the model responds (`0` = off) |

## Layout

| Module | Role |
|--------|------|
| `bots/game.py` | pgzero entry, pygame_gui events |
| `bots/game_logic.py` | Map, tools (`MoveTo`, `Look*`, `Dig`, `Create`), flares, power |
| `bots/rendering.py` | Viewport, UI windows, start menu |
| `bots/ollama_agent.py` | Ollama chat + tool loop |
| `bots/message_log.py` | `print` capture for log window |
| `bots/cli.py` | `poetry run bots` → pgzero |

**Stack:** Python 3.12+, pgzero, pygame-gui, `ollama` client.

## License

MIT — see [LICENSE](LICENSE).
