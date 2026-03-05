from pathlib import Path
import subprocess
import sys


def main() -> None:
    game_file = Path(__file__).with_name("game.py")
    subprocess.run([sys.executable, "-m", "pgzero", str(game_file)], check=False)
