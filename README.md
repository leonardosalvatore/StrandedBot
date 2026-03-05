# Bots

A small Pygame Zero game where you control a blue player ball with `W`, `A`, `S`, `D` on a `600x600` map.

## Requirements

- Python (managed by Poetry)
- Ollama running locally for AI scenery generation

## Install

```bash
poetry install
```

## Run

```bash
poetry run bots
```

## Ollama scenery

By default, the game asks Ollama to generate tiles through one tool:

- `CreateTile(x, y, type)` where `type` is one of `grass`, `sand`, `water`, `forest`, `home`, `road`

Environment variables:

- `OLLAMA_MODEL` (default: `llama3.2`)
- `OLLAMA_SCENERY=0` to disable Ollama map generation and keep default grass map
