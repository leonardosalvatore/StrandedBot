"""Start screen: model, resources, solar interval, interactive mode, custom prompt."""

from typing import Any

import pygame
import pygame_gui
from pygame_gui import UI_BUTTON_PRESSED
from pygame_gui.elements import UIButton, UILabel, UIWindow, UITextEntryBox

from bots import game_logic

_ui_manager: pygame_gui.UIManager | None = None

_start_window: UIWindow | None = None
_start_default_button: UIButton | None = None
_start_custom_button: UIButton | None = None
_interactive_mode_checkbox: UIButton | None = None
_start_model_entry: UITextEntryBox | None = None
_start_rocks_entry: UITextEntryBox | None = None
_start_energy_entry: UITextEntryBox | None = None
_start_inventory_entry: UITextEntryBox | None = None
_start_solar_flare_entry: UITextEntryBox | None = None
_start_initial_town_entry: UITextEntryBox | None = None
_start_ant_progression_entry: UITextEntryBox | None = None
_start_spawn_ant_after_hour_entry: UITextEntryBox | None = None
_start_ant_hits_entry: UITextEntryBox | None = None
_start_turret_bullet_rate_entry: UITextEntryBox | None = None

_start_explorer_button: UIButton | None = None
_start_tower_defense_button: UIButton | None = None
_start_builder_button: UIButton | None = None
_selected_scenario = "Explorer"
_interactive_mode_enabled = True

_custom_prompt_window: UIWindow | None = None
_custom_prompt_entry: UITextEntryBox | None = None
_custom_prompt_confirm_button: UIButton | None = None
_custom_prompt_cancel_button: UIButton | None = None


def set_ui_manager(manager: pygame_gui.UIManager | None) -> None:
    global _ui_manager
    _ui_manager = manager


def create_start_menu(screen_size: tuple[int, int], default_model: str) -> None:
    global _start_window, _start_default_button, _start_custom_button
    global _interactive_mode_checkbox, _start_model_entry, _start_rocks_entry, _interactive_mode_enabled
    global _start_energy_entry, _start_inventory_entry, _start_solar_flare_entry
    global _start_initial_town_entry
    global _start_ant_progression_entry, _start_spawn_ant_after_hour_entry
    global _start_ant_hits_entry, _start_turret_bullet_rate_entry
    global _start_explorer_button, _start_tower_defense_button, _start_builder_button, _selected_scenario
    if _ui_manager is None:
        print("[DEBUG] Cannot create start menu - UI manager is None")
        return

    win_w, win_h = 600, 670
    label_w = 340
    entry_x, entry_w = 370, 200
    x = (screen_size[0] - win_w) // 2
    y = (screen_size[1] - win_h) // 2
    _start_window = UIWindow(
        rect=pygame.Rect((x, y), (win_w, win_h)),
        manager=_ui_manager,
        window_display_title="Start Game",
        resizable=False,
        draggable=False,
    )
    _selected_scenario = _selected_scenario if _selected_scenario in {"Explorer", "Tower Defense", "Builder"} else "Explorer"
    scenario_options = ["Explorer", "Tower Defense", "Builder"]
    UILabel(
        relative_rect=pygame.Rect((20, 10), (120, 30)),
        text="Scenario:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_explorer_button = UIButton(
        relative_rect=pygame.Rect((140, 10), (140, 30)),
        text="Explorer",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_tower_defense_button = UIButton(
        relative_rect=pygame.Rect((285, 10), (140, 30)),
        text="Tower Defense",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_builder_button = UIButton(
        relative_rect=pygame.Rect((430, 10), (140, 30)),
        text="Builder",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_default_button = UIButton(
        relative_rect=pygame.Rect((20, 50), (390, 40)),
        text="Start Bot with default prompt",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_custom_button = UIButton(
        relative_rect=pygame.Rect((20, 100), (390, 40)),
        text="Start Bot with Custom prompt",
        manager=_ui_manager,
        container=_start_window,
    )
    UILabel(
        relative_rect=pygame.Rect((20, 150), (label_w, 30)),
        text="OLLAMA_MODEL:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_model_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 150), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_model_entry.set_text(default_model or game_logic.OLLAMA_MODEL)
    UILabel(
        relative_rect=pygame.Rect((20, 190), (label_w, 30)),
        text="Rocks clusters generated:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_rocks_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 190), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_rocks_entry.set_text(str(game_logic.STARTING_WORLD_ROCKS_TARGET))
    UILabel(
        relative_rect=pygame.Rect((20, 230), (label_w, 30)),
        text="Initial town (habitat groups):",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_initial_town_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 230), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_initial_town_entry.set_text(str(int(game_logic.STARTING_INITIAL_TOWN_SIZE)))
    UILabel(
        relative_rect=pygame.Rect((20, 270), (label_w, 30)),
        text="Energy:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_energy_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 270), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_energy_entry.set_text(str(game_logic.STARTING_BOT_ENERGY))
    UILabel(
        relative_rect=pygame.Rect((20, 310), (label_w, 30)),
        text="Inventory rocks:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_inventory_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 310), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_inventory_entry.set_text(str(game_logic.STARTING_INVENTORY_ROCKS))
    UILabel(
        relative_rect=pygame.Rect((20, 350), (label_w, 30)),
        text="Hours between solar flares:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_solar_flare_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 350), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_solar_flare_entry.set_text(str(game_logic.STARTING_HOURS_SOLAR_FLARE_EVERY))
    UILabel(
        relative_rect=pygame.Rect((20, 390), (label_w, 30)),
        text="Ants spawned / game hour:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_ant_progression_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 390), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_ant_progression_entry.set_text(str(int(game_logic.STARTING_ANT_PROGRESSION)))
    UILabel(
        relative_rect=pygame.Rect((20, 425), (label_w, 30)),
        text="Spawn ants after game hour:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_spawn_ant_after_hour_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 425), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_spawn_ant_after_hour_entry.set_text(str(int(game_logic.STARTING_SPAWN_ANT_AFTER_HOUR)))
    UILabel(
        relative_rect=pygame.Rect((20, 460), (label_w, 30)),
        text="Laser hits to kill ant:",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_ant_hits_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 460), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_ant_hits_entry.set_text(str(int(game_logic.STARTING_ANT_HITS_TO_KILL)))
    UILabel(
        relative_rect=pygame.Rect((20, 495), (label_w, 30)),
        text="Turret shots/sec (real):",
        manager=_ui_manager,
        container=_start_window,
    )
    _start_turret_bullet_rate_entry = UITextEntryBox(
        relative_rect=pygame.Rect((entry_x, 495), (entry_w, 35)),
        manager=_ui_manager,
        container=_start_window,
    )
    _start_turret_bullet_rate_entry.set_text(str(float(game_logic.STARTING_TURRET_BULLET_RATE)))
    _interactive_mode_checkbox = UIButton(
        relative_rect=pygame.Rect((20, 540), (550, 40)),
        text="Interactive mode, you can reply to Bot question.",
        manager=_ui_manager,
        container=_start_window,
    )
    _interactive_mode_enabled = True

    _refresh_scenario_buttons()
    _apply_scenario_defaults(_selected_scenario)


def _apply_scenario_defaults(scenario: str) -> None:
    """Overwrite all gameplay fields from the selected scenario."""
    global _start_rocks_entry, _start_initial_town_entry, _start_energy_entry, _start_inventory_entry
    global _start_solar_flare_entry, _start_ant_progression_entry, _start_spawn_ant_after_hour_entry
    global _start_ant_hits_entry, _start_turret_bullet_rate_entry

    presets: dict[str, dict[str, Any]] = {
        # Explorer: more energy/time to roam + start with a small base.
        "Explorer": {
            "rocks_amount": 20,
            "initial_town_size": 0,
            "energy": 1000,
            "inventory_rocks": 10,
            "hours_solar_flare_every": 100,
            "ant_progression": 0,
            "spawn_ant_after_hour": 100,
            "ant_hits_to_kill": game_logic.STARTING_ANT_HITS_TO_KILL,
            "turret_bullet_rate": game_logic.STARTING_TURRET_BULLET_RATE,
        },
        # Tower Defense: defend the tiny initial town more often.
        "Tower Defense": {
            "rocks_amount": 10,
            "initial_town_size": 2,
            "energy": 100,
            "inventory_rocks": 20,
            "hours_solar_flare_every": 1200,
            "ant_progression": 1,
            "spawn_ant_after_hour": 40,
            "ant_hits_to_kill": max(1, game_logic.STARTING_ANT_HITS_TO_KILL - 1),
            "turret_bullet_rate": 3,
        },
        # Builder: lower pressure, high energy and lots of rocks for expansion.
        "Builder": {
            "rocks_amount": 100,
            "initial_town_size": 0,
            "energy": 1000,
            "inventory_rocks": 100,
            "hours_solar_flare_every": 3000,
            "ant_progression": 0,
            "spawn_ant_after_hour": 0,
            "ant_hits_to_kill": game_logic.STARTING_ANT_HITS_TO_KILL + 1,
            "turret_bullet_rate": 0.9,
        },
    }

    preset = presets.get(scenario)
    if not preset:
        return

    if _start_rocks_entry is not None:
        _start_rocks_entry.set_text(str(preset["rocks_amount"]))
    if _start_initial_town_entry is not None:
        _start_initial_town_entry.set_text(str(int(preset["initial_town_size"])))
    if _start_energy_entry is not None:
        _start_energy_entry.set_text(str(int(preset["energy"])))
    if _start_inventory_entry is not None:
        _start_inventory_entry.set_text(str(int(preset["inventory_rocks"])))
    if _start_solar_flare_entry is not None:
        _start_solar_flare_entry.set_text(str(int(preset["hours_solar_flare_every"])))
    if _start_ant_progression_entry is not None:
        _start_ant_progression_entry.set_text(str(int(preset["ant_progression"])))
    if _start_spawn_ant_after_hour_entry is not None:
        _start_spawn_ant_after_hour_entry.set_text(str(int(preset["spawn_ant_after_hour"])))
    if _start_ant_hits_entry is not None:
        _start_ant_hits_entry.set_text(str(int(preset["ant_hits_to_kill"])))
    if _start_turret_bullet_rate_entry is not None:
        _start_turret_bullet_rate_entry.set_text(str(float(preset["turret_bullet_rate"])))


def _refresh_scenario_buttons() -> None:
    if _start_explorer_button is not None:
        _start_explorer_button.set_text("[Explorer]" if _selected_scenario == "Explorer" else "Explorer")
    if _start_tower_defense_button is not None:
        _start_tower_defense_button.set_text(
            "[Tower Defense]" if _selected_scenario == "Tower Defense" else "Tower Defense"
        )
    if _start_builder_button is not None:
        _start_builder_button.set_text("[Builder]" if _selected_scenario == "Builder" else "Builder")


def _open_custom_prompt_dialog() -> None:
    global _custom_prompt_window, _custom_prompt_entry
    global _custom_prompt_confirm_button, _custom_prompt_cancel_button
    if _ui_manager is None or _custom_prompt_window is not None:
        return

    _custom_prompt_window = UIWindow(
        rect=pygame.Rect((140, 120), (920, 520)),
        manager=_ui_manager,
        window_display_title="Custom Prompt",
        resizable=False,
    )
    UILabel(
        relative_rect=pygame.Rect((15, 10), (880, 25)),
        text="Enter the prompt text for the AI:",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_entry = UITextEntryBox(
        relative_rect=pygame.Rect((15, 40), (880, 370)),
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_confirm_button = UIButton(
        relative_rect=pygame.Rect((15, 430), (430, 50)),
        text="Start with custom prompt",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )
    _custom_prompt_cancel_button = UIButton(
        relative_rect=pygame.Rect((465, 430), (430, 50)),
        text="Cancel",
        manager=_ui_manager,
        container=_custom_prompt_window,
    )


def open_custom_prompt_dialog(default_text: str) -> None:
    _open_custom_prompt_dialog()
    if _custom_prompt_entry is not None:
        _custom_prompt_entry.set_text(default_text)
    pygame.key.start_text_input()


def _close_custom_prompt_dialog() -> None:
    global _custom_prompt_window, _custom_prompt_entry
    global _custom_prompt_confirm_button, _custom_prompt_cancel_button
    if _custom_prompt_window is not None:
        _custom_prompt_window.kill()
    _custom_prompt_window = None
    _custom_prompt_entry = None
    _custom_prompt_confirm_button = None
    _custom_prompt_cancel_button = None
    pygame.key.stop_text_input()


def _close_start_menu() -> None:
    global _start_window, _start_default_button, _start_custom_button
    global _interactive_mode_checkbox, _start_model_entry, _start_rocks_entry
    global _start_energy_entry, _start_inventory_entry, _start_solar_flare_entry
    global _start_initial_town_entry
    global _start_ant_progression_entry, _start_spawn_ant_after_hour_entry
    global _start_ant_hits_entry, _start_turret_bullet_rate_entry
    global _start_explorer_button, _start_tower_defense_button, _start_builder_button, _selected_scenario
    if _start_window is not None:
        _start_window.kill()
    _start_window = None
    _start_default_button = None
    _start_custom_button = None
    _interactive_mode_checkbox = None
    _start_model_entry = None
    _start_rocks_entry = None
    _start_energy_entry = None
    _start_inventory_entry = None
    _start_solar_flare_entry = None
    _start_initial_town_entry = None
    _start_ant_progression_entry = None
    _start_spawn_ant_after_hour_entry = None
    _start_ant_hits_entry = None
    _start_turret_bullet_rate_entry = None
    _start_explorer_button = None
    _start_tower_defense_button = None
    _start_builder_button = None
    _selected_scenario = "Explorer"
    _close_custom_prompt_dialog()


def handle_startup_ui_event(event: pygame.event.Event) -> dict[str, Any] | None:
    global _interactive_mode_enabled, _selected_scenario

    def _read_rocks_amount() -> int:
        default = game_logic.STARTING_WORLD_ROCKS_TARGET
        if _start_rocks_entry is None:
            return default
        raw = _start_rocks_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(1, value)

    def _read_energy() -> int:
        default = game_logic.STARTING_BOT_ENERGY
        if _start_energy_entry is None:
            return default
        raw = _start_energy_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(1, value)

    def _read_inventory_rocks() -> int:
        default = game_logic.STARTING_INVENTORY_ROCKS
        if _start_inventory_entry is None:
            return default
        raw = _start_inventory_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(0, value)

    def _read_hours_solar_flare_every() -> int:
        default = game_logic.STARTING_HOURS_SOLAR_FLARE_EVERY
        if _start_solar_flare_entry is None:
            return default
        raw = _start_solar_flare_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(1, value)

    def _read_ant_progression() -> int:
        default = int(game_logic.STARTING_ANT_PROGRESSION)
        if _start_ant_progression_entry is None:
            return default
        raw = _start_ant_progression_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(0, value)

    def _read_spawn_ant_after_hour() -> int:
        default = int(game_logic.STARTING_SPAWN_ANT_AFTER_HOUR)
        if _start_spawn_ant_after_hour_entry is None:
            return default
        raw = _start_spawn_ant_after_hour_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(0, value)

    def _read_initial_town_size() -> int:
        default = int(game_logic.STARTING_INITIAL_TOWN_SIZE)
        if _start_initial_town_entry is None:
            return default
        raw = _start_initial_town_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(0, min(12, value))

    def _read_ant_hits_to_kill() -> int:
        default = int(game_logic.STARTING_ANT_HITS_TO_KILL)
        if _start_ant_hits_entry is None:
            return default
        raw = _start_ant_hits_entry.get_text().strip()
        try:
            value = int(raw)
        except (TypeError, ValueError):
            return default
        return max(1, value)

    def _read_turret_bullet_rate() -> float:
        default = float(game_logic.STARTING_TURRET_BULLET_RATE)
        if _start_turret_bullet_rate_entry is None:
            return default
        raw = _start_turret_bullet_rate_entry.get_text().strip()
        try:
            value = float(raw)
        except (TypeError, ValueError):
            return default
        return max(0.05, value)

    if event.type == UI_BUTTON_PRESSED:
        if _start_explorer_button and event.ui_element == _start_explorer_button:
            _selected_scenario = "Explorer"
            _refresh_scenario_buttons()
            _apply_scenario_defaults(_selected_scenario)
            print("[StartMenu] Scenario button selected: Explorer")
            return None

        if _start_tower_defense_button and event.ui_element == _start_tower_defense_button:
            _selected_scenario = "Tower Defense"
            _refresh_scenario_buttons()
            _apply_scenario_defaults(_selected_scenario)
            print("[StartMenu] Scenario button selected: Tower Defense")
            return None

        if _start_builder_button and event.ui_element == _start_builder_button:
            _selected_scenario = "Builder"
            _refresh_scenario_buttons()
            _apply_scenario_defaults(_selected_scenario)
            print("[StartMenu] Scenario button selected: Builder")
            return None

        if _start_default_button and event.ui_element == _start_default_button:
            model_name = game_logic.OLLAMA_MODEL
            if _start_model_entry is not None:
                model_name = _start_model_entry.get_text().strip() or game_logic.OLLAMA_MODEL
            rocks_amount = _read_rocks_amount()
            initial_town_size = _read_initial_town_size()
            energy = _read_energy()
            inventory_rocks = _read_inventory_rocks()
            hours_solar_flare_every = _read_hours_solar_flare_every()
            ant_progression = _read_ant_progression()
            spawn_ant_after_hour = _read_spawn_ant_after_hour()
            ant_hits_to_kill = _read_ant_hits_to_kill()
            turret_bullet_rate = _read_turret_bullet_rate()
            _close_start_menu()
            return {
                "action": "start_default",
                "interactive_mode": _interactive_mode_enabled,
                "scenario": _selected_scenario,
                "model": model_name,
                "rocks_amount": rocks_amount,
                "initial_town_size": initial_town_size,
                "energy": energy,
                "inventory_rocks": inventory_rocks,
                "hours_solar_flare_every": hours_solar_flare_every,
                "ant_progression": ant_progression,
                "spawn_ant_after_hour": spawn_ant_after_hour,
                "ant_hits_to_kill": ant_hits_to_kill,
                "turret_bullet_rate": turret_bullet_rate,
            }

        if _start_custom_button and event.ui_element == _start_custom_button:
            return {"action": "open_custom", "rocks_amount": _read_rocks_amount(), "scenario": _selected_scenario}

        if _interactive_mode_checkbox and event.ui_element == _interactive_mode_checkbox:
            _interactive_mode_enabled = not _interactive_mode_enabled
            if _interactive_mode_enabled:
                _interactive_mode_checkbox.set_text("Interactive mode, you can reply to Bot question.")
            else:
                _interactive_mode_checkbox.set_text("Not interactive mode, Bot is all alone")
            return None

        if _custom_prompt_cancel_button and event.ui_element == _custom_prompt_cancel_button:
            _close_custom_prompt_dialog()
            return {"action": "cancel_custom"}

        if _custom_prompt_confirm_button and event.ui_element == _custom_prompt_confirm_button:
            prompt_text = ""
            if _custom_prompt_entry is not None:
                prompt_text = _custom_prompt_entry.get_text().strip()

            model_name = game_logic.OLLAMA_MODEL
            if _start_model_entry is not None:
                model_name = _start_model_entry.get_text().strip() or game_logic.OLLAMA_MODEL
            rocks_amount = _read_rocks_amount()
            initial_town_size = _read_initial_town_size()
            energy = _read_energy()
            inventory_rocks = _read_inventory_rocks()
            hours_solar_flare_every = _read_hours_solar_flare_every()
            ant_progression = _read_ant_progression()
            spawn_ant_after_hour = _read_spawn_ant_after_hour()
            ant_hits_to_kill = _read_ant_hits_to_kill()
            turret_bullet_rate = _read_turret_bullet_rate()
            if not prompt_text:
                return {"action": "custom_prompt_empty"}
            _close_start_menu()
            return {
                "action": "start_custom",
                "prompt": prompt_text,
                "interactive_mode": _interactive_mode_enabled,
                "scenario": _selected_scenario,
                "model": model_name,
                "rocks_amount": rocks_amount,
                "initial_town_size": initial_town_size,
                "energy": energy,
                "inventory_rocks": inventory_rocks,
                "hours_solar_flare_every": hours_solar_flare_every,
                "ant_progression": ant_progression,
                "spawn_ant_after_hour": spawn_ant_after_hour,
                "ant_hits_to_kill": ant_hits_to_kill,
                "turret_bullet_rate": turret_bullet_rate,
            }

    return None


def _clamp_window_to_screen(window: UIWindow | None, screen_size: tuple[int, int]) -> None:
    if window is None:
        return
    rect = window.get_abs_rect()
    max_x = max(0, screen_size[0] - rect.width)
    max_y = max(0, screen_size[1] - rect.height)
    new_x = max(0, min(rect.x, max_x))
    new_y = max(0, min(rect.y, max_y))
    if new_x != rect.x or new_y != rect.y:
        window.set_position((new_x, new_y))


def clamp_start_menu_windows(screen_size: tuple[int, int]) -> None:
    """Keep start and custom-prompt windows on screen after resize."""
    _clamp_window_to_screen(_start_window, screen_size)
    _clamp_window_to_screen(_custom_prompt_window, screen_size)
