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

## How LLM ↔ tools ↔ world fit

**`game_logic`** owns the map, bot stats, solar flares, and the real implementations: `MoveTo`, `LookClose`, `LookFar`, `Dig`, `Create`. Each returns a dict (e.g. `ok`, positions, energy) and mutates shared state. `get_tool_dispatch()` maps tool names → those callables.

**`ollama_agent`** builds the **tool schema** (names, descriptions, JSON parameters) to match that API, runs **`ollama.chat(..., tools=...)`**, reads `tool_calls` from the assistant message, invokes the matching `game_logic` function with parsed arguments, and appends **tool results** (JSON strings) to the chat history so the next model turn sees outcomes.

The **main pygame loop** only draws and UI; the **Ollama worker thread** runs this turn loop so the game can keep updating while the model thinks (optional sim-time ticks via `BOTS_SIM_HOUR_WALL_SEC`).

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
