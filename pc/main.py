"""
Entry point for the desktop app.

Run with:  python main.py
"""

import logging
import sys
from pathlib import Path

from PySide6.QtWidgets import QApplication

from ui import MainWindow


def configure_logging():
    """
    Basic logging setup: everything goes to both the console and a log
    file next to this script, so serial issues can be diagnosed after
    the fact instead of only being visible while the app is running.
    """
    log_dir = Path(__file__).resolve().parent / "logs"
    log_dir.mkdir(exist_ok=True)

    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        handlers=[
            logging.FileHandler(log_dir / "desktop_app.log"),
            logging.StreamHandler(sys.stdout),
        ],
    )


def main():
    configure_logging()
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
