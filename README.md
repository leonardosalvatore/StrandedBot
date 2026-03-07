# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**bots** is a Pygame Zero game where you control a blue player ball moving on a 1000x800 grid map. The map is generated procedurally with lakes, forests, villages, and roads. Optionally, an Ollama LLM can autonomously control the player using tool-calling to explore the map.

## Commands

### Build/Install
```bash
poetry install
```

### Run the Game
```bash
poetry run bots
```

### Run a Single Test (if tests exist)
```bash
poetry run pytest <test_name>  # or poetry run python -m pytest ...
```

## Architecture

### Core Files

- **`bots/game.py`** - Thin Pygame Zero entrypoint/orchestrator:
  - Initializes world and optional Ollama play thread
  - Exposes `update(dt)` and `draw()` expected by `pgzero`

- **`bots/game_logic.py`** - Core game state and mechanics:
  - Tile/map data structures and procedural generation
  - Bot actions and tool functions (`MoveTo`, `LookClose`, `LookFar`, `OpenCrate`, `TakeAllFromCrate`)
  - Movement update loop and status helpers

- **`bots/rendering.py`** - Drawing/UI layer:
  - Camera and map rendering
  - Bot sprite rendering and HUD/speech panel

- **`bots/ollama_agent.py`** - Ollama integration:
  - Prompt/tool schema construction
  - Tool-calling loop and model lifecycle helpers

- **`bots/cli.py`** - CLI entry point:
  - Launches game via `pgzrun`

### Dependencies

- **pgzero** - Pygame Zero game framework (v1.2.1+)
- **ollama** - Python client for Ollama AI (v0.6.1+)

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `OLLAMA_MODEL` | `qwen3.5:9b` | Ollama model for LLM play mode |
| `OLLAMA_PLAY` | `0` | Enable LLM autonomous play (set to 1 to enable) |

### Tile System

- Grid dimensions: 1000x800 pixels
- Tile size: 10x10 pixels
- Grid size: 100 columns x 80 rows
- `Tile` dataclass: x, y, type, color (RGB tuple), description (str)
- `tile_matrix[x][y]` - 2D list of `Tile` objects
- Thread-safe tile dictionary + matrix with `tiles_lock` Lock

### Tile Types and Colors

| Type | Color (RGB) |
|------|-------------|
| grass | (80, 170, 80) |
| sand | (220, 200, 120) |
| water | (70, 130, 220) |
| forest | (30, 110, 30) |
| home | (190, 120, 90) |
| road | (120, 120, 120) |

### Ollama Integration

The game uses Ollama's tool-calling API to let an LLM autonomously explore the map. When `OLLAMA_PLAY=1`, a daemon thread runs `_run_ollama_play_loop()` which:

1. Sends a base prompt describing the world and goals
2. The LLM calls `Move(direction)` or `GetInfo()` tools
3. Tool results are fed back as `role: "tool"` messages
4. The loop continues indefinitely (the LLM keeps exploring)

**LLM Tools:**

| Tool | Description |
|------|-------------|
| `Move(direction)` | Move player one tile step in 8 directions (north, south, east, west, northeast, northwest, southeast, southwest) |
| `GetInfo()` | Returns 3x3 grid of tiles around the player with coordinates, type, and description |

### Key Functions

| Function | Purpose |
|----------|---------|
| `CreateTile(x, y, type)` | Thread-safe tile placement (returns dict with status) |
| `Move(direction)` | Move player in one of 8 compass directions |
| `GetInfo()` | Read surrounding tile info (3x3 grid) |
| `_run_ollama_play_loop()` | LLM agent loop with tool calling |
| `_build_scenery_procedural()` | Fast procedural map generation |
| `_start_scenery_generation()` | Init map and optionally start LLM play |

## Implementation Notes

- Player movement uses fixed timestep with `PLAYER_SPEED = 220` pixels/second
- Player bounds-checked against PLAYER_RADIUS (12px)
- Tile updates are protected by threading lock to ensure data consistency during Ollama requests
- Map is always generated procedurally first, then LLM play starts if enabled
- LLM play loop runs in a daemon thread so it doesn't block the game
