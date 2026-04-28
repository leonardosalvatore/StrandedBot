# bots

A local LLM plays a small tile survival game: a robot explores a grid world,
scans terrain, digs rocks, builds habitats / batteries / solar panels, and
tries to stay charged through periodic solar flares. The game runs in native
C with raylib; the model runs locally via `llama.cpp`'s OpenAI-compatible
server.

[Short video demo (YouTube)](https://www.youtube.com/shorts/6EPWKLKFVEY) ·

## Requirements

- A C17 compiler (gcc / clang)
- CMake ≥ 3.20 and `make`
- Network + git (CMake's `FetchContent` pulls raylib 5.5 and raygui 4.0 on
  first build)
- X11 / Wayland dev headers that raylib needs on Linux
  (`libxrandr`, `libxinerama`, `libxcursor`, `libxi`, `libgl1-mesa-dev`, …)
- A [`llama.cpp`](https://github.com/ggerganov/llama.cpp) build of
  `llama-server` and any tool-calling-capable GGUF model (tested with
  `Ministral-3-8B-Reasoning` and Qwen 2.5 variants)

Optional: a monospace TTF somewhere on the system (Ubuntu Mono, DejaVu
Sans Mono, or Liberation Mono). The HUD auto-detects one of these and
falls back to raylib's bitmap font if none is found.

## Build

```bash
cd bots-c
cmake -S . -B build
cmake --build build -j
```

The resulting executable is `bots-c/build/bots`.

## Run

The game can spawn `llama-server` itself — by default the start menu has
**AUTO-START LLAMA SERVER** checked, so launching `bots` is usually all
you need:

```bash
./bots-c/build/bots
```

On first start, the game reads `bots-defaults.json` (and `bots-custom.json`
if it exists), forks the script pointed at by `llama.start_script`
(defaults to `./start-llama-server.sh`) if the port isn't already open,
inherits its stdout/stderr, and sends it SIGTERM on exit. Hitting Ctrl-C
in the terminal also tears down the child.

If you'd rather run the server yourself (for example, under a debugger or
on a different host), uncheck **AUTO-START LLAMA SERVER** in the menu and
run it in another terminal:

```bash
./start-llama-server.sh [/path/to/model.gguf]
```

The script starts `llama-server` on `127.0.0.1:<PORT>` (default `53425`;
pass `-p <PORT>` or set `LLAMA_PORT=<PORT>` to override) with:

- `-c 16384` — 16k context (reasoning models burn through 8k quickly)
- `--jinja` — use the GGUF's embedded chat template (required for
  tool-calling / reasoning models)
- `--reasoning-format deepseek` — hide the model's internal `<think>…</think>`
  trace from the client

The bundled script also sets up ROCm library paths for AMD GPUs. Edit it
(`LLAMA_DIR`, `DEFAULT_MODEL`, the `HIP_*` env vars) to point at your own
binaries, or just run `llama-server` yourself with the same flags — the
client only cares that there's an OpenAI-compatible endpoint at
`http://127.0.0.1:<llama.port>/v1/chat/completions` (default `53425`).

The start menu is a single flat preset (no scenario picker, no
default-prompt option, no interactive-mode toggle):

- **Rocks amount** — density of rock clusters in the procedural map
- **Initial town size** — pre-placed habitat cluster (0 to disable)
- **Starting energy** and **starting inventory rocks**
- **Hours between solar flares**
- **llama start script** — path to the launcher script the game execs
- **Auto-start llama server** — fork the script above if nothing is
  already listening on `:<llama.port>`
- **Custom prompt (optional)** — extra mission text prepended to the
  conversation; leave it empty and the bot just runs against the system
  prompt + per-turn telemetry
- **Revert to defaults** — overwrite `bots-custom.json` with
  `bots-defaults.json` and reload the menu

The bot pauses for a typed reply whenever its message contains a `?`,
and the always-visible compose box lets you queue a user message for the
bot at any time.

Every widget state is written to `bots-custom.json` the instant you hit
RUN, so the next launch (or `--autostart-custom`) replays it.

`F11` toggles fullscreen. Pan with right-mouse drag, zoom with the
scroll wheel.

### CLI flags

| Flag                   | Effect                                                   |
|------------------------|----------------------------------------------------------|
| `--autostart-default`  | Skip the start menu; use values from `bots-defaults.json`|
| `--autostart-custom`   | Skip the start menu; use values from `bots-custom.json`  |
| `-h`, `--help`         | Print usage                                              |

Handy for rebuild-run-repeat loops while debugging.

## Configuration

Two JSON files (searched in the current working directory, then next to
the `bots` executable) hold every tunable knob, including the full LLM
prompt set:

- **`bots-defaults.json`** — shipped with the repo, read-only baseline.
  Includes the agent `SYSTEM_PROMPT`, every per-tool description
  (`MoveTo`, `LookFar`, `Dig`, `Create`, `ListBuiltTiles`), the llama
  launcher script path, and the single game preset (rocks, energy,
  inventory, flare interval, …).
- **`bots-custom.json`** — user-writable overrides. Created the first
  time you hit RUN in the start menu; rewritten on every subsequent run.
  You can also hand-edit it.

Overlay rules: `bots-custom.json` wins over `bots-defaults.json` for any
field it defines; missing fields fall back to the defaults. If neither
file is present, a hardcoded fallback matching the shipped defaults is
used so the binary runs even when invoked outside the repo.

Tool description strings may contain `%d` placeholders (filled at runtime
with grid size, move range, rock costs). Preserve their order when
editing.

The **REVERT TO DEFAULTS** button in the start menu rewrites
`bots-custom.json` with the contents of `bots-defaults.json` and reloads
all widgets.

## How LLM ↔ tools ↔ world fit

**`game_logic.c`** owns the 2D tile matrix, bot stats (energy, inventory,
position), solar-flare schedule, and the real implementations of the
tools: `MoveTo`, `LookFar`, `Dig`, `Create`, `ListBuiltTiles`. Each tool
mutates shared state behind `gl_tiles_lock` / `gl_built_lock` and returns
a `ToolResult` containing a JSON string (`ok`, new position, energy,
`hours_to_solar_flare`, and diagnostic fields like `previous_tile_type`
or a `note` when relevant).

**`llm_agent.c`** runs the model loop on a background thread:

1. Builds an OpenAI-style `tools` array describing each game function
   and its JSON parameters (`build_tools_json`).
2. Prepends a `SYSTEM STATUS` line every turn with the current hour,
   energy, position, `current_tile_buildable`, inventory, flare ETA,
   and a diagnosed `habitat_hourly_charge_active` reason. That way the
   model doesn't have to remember prior state.
3. Calls `http_post` → `POST /v1/chat/completions` on
   `127.0.0.1:<llama.port>`, parses `tool_calls` from the assistant message,
   dispatches them via `gl_dispatch_tool_call`, and appends the JSON
   result as a `tool` message for the next turn.
4. `--reasoning-format deepseek` means the server strips the
   `<think>…</think>` trace; we keep the last 30 hours of chat history
   and feed it back each turn.

**`main.c`** is a classic raylib loop: draw the game, update particles,
poll input, render the HUD (stats panel on the top-left, scrollable
`SYSLOG` at the bottom). The LLM thread runs independently; the game
clock is driven by the agent loop taking a turn, not by real time.

### Tool API (what the model sees)

| Tool            | Effect                                                                 | Cost            |
|-----------------|------------------------------------------------------------------------|-----------------|
| `MoveTo(x,y)`   | Pathfind to `(x,y)`. Advances one hour per tile of path.               | 1 energy / tile |
| `LookFar`       | Scans the surrounding area, clears fog of war, returns notable tiles.  | 1 energy        |
| `Dig`           | Consumes a `rocks` tile under the bot → `+1 rocks` inventory, tile becomes gravel. | 1 energy  |
| `Create(type)`  | Builds `habitat`, `battery`, or `solar_panel` on current buildable tile. | 1 energy + rock cost |
| `ListBuiltTiles`| Returns every structure the bot has placed this run.                   | 1 energy        |

Buildable ground is gravel, sand, or rocks (creating on rocks consumes
that rocks tile permanently). Power requires orthogonal adjacency
(`|dx|+|dy|=1`) between at least one habitat, one battery, and one
solar_panel; the bot only charges while standing on a habitat in such a
cluster.

## Layout

| File                  | Role                                                     |
|-----------------------|----------------------------------------------------------|
| `bots-c/src/main.c`      | Raylib main loop, HUD panels, reply dialog           |
| `bots-c/src/game_logic.c`| Map, tools, solar flares, power network, particle spawns |
| `bots-c/src/rendering.c` | Viewport camera, tile / bot drawing                  |
| `bots-c/src/particles.c` | Scan pulses, build sparkles, dig dust, flare zaps    |
| `bots-c/src/start_menu.c`| Start screen (raygui), preset fields + custom prompt |
| `bots-c/src/ui_theme.c`  | Hacker-terminal raygui theme + TTF font loader       |
| `bots-c/src/llm_agent.c` | OpenAI-compatible chat + tool-call loop              |
| `bots-c/src/llama_launcher.c` | fork/exec + lifecycle for `llama-server`        |
| `bots-c/src/config.c`    | JSON-backed settings and prompts (defaults + custom) |
| `bots-c/src/http_post.c` | Minimal sockets-based HTTP POST                      |
| `bots-c/src/cJSON.c`     | Vendored cJSON parser                                |
| `bots-c/src/message_log.c`| Ring buffer that feeds the SYSLOG panel             |
| `start-llama-server.sh`  | Convenience launcher for `llama-server`              |

**Stack:** C17, raylib 5.5, raygui 4.0, pthreads, cJSON, llama.cpp server.

## License

MIT — see [LICENSE](LICENSE).
