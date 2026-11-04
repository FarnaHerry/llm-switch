#!/usr/bin/env python3
"""Register this checkout's run.sh and current logo for the current Linux user."""

from datetime import datetime
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def desktop_string(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def exec_argument(value):
    quoted = "".join("\\" + c if c in '\\`"$' else c for c in str(value))
    return desktop_string('"' + quoted + '"')


def update_entry(text, launcher, icon):
    values = {"Exec": exec_argument(launcher), "TryExec": desktop_string(launcher), "Icon": desktop_string(icon)}
    lines = []
    in_entry = False
    for line in text.splitlines():
        if line.startswith("["):
            if in_entry:
                lines.extend(f"{key}={value}" for key, value in values.items())
                values.clear()
            in_entry = line == "[Desktop Entry]"
        key = line.partition("=")[0]
        if in_entry and key in values:
            line = f"{key}={values.pop(key)}"
        lines.append(line)
    lines.extend(f"{key}={value}" for key, value in values.items())
    return "\n".join(lines) + "\n"


def replace_file(path, data, backup, mode=0o644):
    if (path.is_file() and not path.is_symlink() and path.read_bytes() == data
            and path.stat().st_mode & 0o777 == mode):
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        backup.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup / f"{len(list(backup.iterdir())):03d}-{path.name}", follow_symlinks=False)
    with tempfile.TemporaryDirectory(prefix=".llmswitch-", dir=path.parent) as directory:
        staged = Path(directory) / path.name
        staged.write_bytes(data)
        staged.chmod(mode)
        os.replace(staged, path)
    print(f"Updated {path}")


def main():
    if not sys.platform.startswith("linux"):
        raise SystemExit("This registration tool is for Linux.")
    home = Path.home()
    data = Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share")
    config = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config")
    launcher = home / ".local/bin/llm-switch"
    icon = data / "icons/hicolor/scalable/apps/llm-switch.svg"
    entry = data / "applications/llm-switch.desktop"
    backup = data / "llm-switch/desktop-backups" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    if not any((ROOT / build / "llm-switch").is_file() for build in ("build", "build-live2d")):
        raise SystemExit("Build the application before registering its desktop entry.")
    text = update_entry((ROOT / "platform/linux/package/llm-switch.desktop").read_text(), launcher, icon)
    validator = shutil.which("desktop-file-validate")
    if validator:
        with tempfile.TemporaryDirectory(prefix="llmswitch-desktop-") as directory:
            staged = Path(directory) / "llm-switch.desktop"
            staged.write_text(text)
            subprocess.run([validator, str(staged)], check=True)
    replace_file(icon, (ROOT / "platform/linux/package/llm-switch.svg").read_bytes(), backup)
    # run.sh resolves its build relative to $0; execute the real path rather than a symlink.
    launch_script = f'#!/usr/bin/env bash\nexec {shlex.quote(str(ROOT / "run.sh"))} "$@"\n'
    replace_file(launcher, launch_script.encode(), backup, mode=0o755)
    replace_file(entry, text.encode(), backup)
    shortcuts = {home / "Desktop/llm-switch.desktop", home / "桌面/llm-switch.desktop",
                 config / "autostart/llm-switch.desktop"}
    desktop_query = shutil.which("xdg-user-dir")
    if desktop_query:
        result = subprocess.run([desktop_query, "DESKTOP"], capture_output=True, text=True, check=True)
        if result.stdout.strip():
            shortcuts.add(Path(result.stdout.strip()) / "llm-switch.desktop")
    for path in shortcuts:
        if path.is_file():
            replace_file(path, update_entry(path.read_text(), launcher, icon).encode(), backup)
    for name, args in (("update-desktop-database", [str(entry.parent)]),
                       ("gtk-update-icon-cache", ["-f", "-t", str(data / "icons/hicolor")]),
                       ("kbuildsycoca6", ["--noincremental"])):
        tool = shutil.which(name)
        if tool:
            result = subprocess.run([tool, *args], capture_output=True, text=True)
            print(f"{name}: {'refreshed' if result.returncode == 0 else 'refresh unavailable'}")
    print(f"Desktop and command launcher now use {ROOT / 'run.sh'}")
    if backup.exists():
        print(f"Previous user files backed up in {backup}")


if __name__ == "__main__":
    main()
