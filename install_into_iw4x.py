from __future__ import annotations

import shutil
import sys
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    raise SystemExit(1)


def backup_once(path: Path) -> None:
    backup = path.with_suffix(path.suffix + ".pre-mw2-vr.bak")
    if not backup.exists():
        shutil.copy2(path, backup)
        print(f"Backup: {backup}")


def insert_after_line(text: str, marker: str, additions: list[str], label: str) -> str:
    lines = text.splitlines(keepends=True)
    existing = {line.strip() for line in lines}
    if all(item.strip() in existing for item in additions):
        print(f"Already patched: {label}")
        return text

    for index, line in enumerate(lines):
        if line.strip() != marker:
            continue

        if line.endswith("\r\n"):
            eol = "\r\n"
        elif line.endswith("\n"):
            eol = "\n"
        else:
            eol = "\n"

        indent = line[: len(line) - len(line.lstrip())]
        inserted = "".join(indent + item + eol for item in additions)
        lines.insert(index + 1, inserted)
        print(f"Patched: {label}")
        return "".join(lines)

    fail(f"Could not find patch marker for {label}: {marker!r}")
    return text


def main() -> None:
    if len(sys.argv) != 2:
        print(r"Usage: python install_into_iw4x.py C:\path\to\iw4x-client")
        raise SystemExit(2)

    root = Path(sys.argv[1]).expanduser().resolve()
    loader = root / "src" / "Components" / "Loader.cpp"
    premake = root / "premake5.lua"
    modules = root / "src" / "Components" / "Modules"

    if not loader.is_file() or not premake.is_file() or not modules.is_dir():
        fail(f"{root} does not look like an iw4x-client checkout")

    backup_once(loader)
    backup_once(premake)

    for name in ("OpenXR.hpp", "OpenXR.cpp"):
        source = PACKAGE / "src" / "Components" / "Modules" / name
        if not source.is_file():
            fail(f"Missing package source: {source}")
        target = modules / name
        shutil.copy2(source, target)
        print(f"Copied: {target}")

    loader_text = loader.read_text(encoding="utf-8")
    loader_text = insert_after_line(
        loader_text,
        '#include "Modules/Node.hpp"',
        ['#include "Modules/OpenXR.hpp"'],
        "Loader include",
    )
    loader_text = insert_after_line(
        loader_text,
        "Register(new Scheduler());",
        ["Register(new OpenXR());"],
        "OpenXR component registration",
    )
    loader.write_text(loader_text, encoding="utf-8", newline="")

    premake_text = premake.read_text(encoding="utf-8")
    premake_text = insert_after_line(
        premake_text,
        "dependencies.imports()",
        [
            'libdirs { "./lib/openxr/win32" }',
            'links { "d3d11", "dxgi", "openxr_loader" }',
        ],
        "OpenXR/D3D11 linker settings",
    )
    premake.write_text(premake_text, encoding="utf-8", newline="")

    setup_src = PACKAGE / "tools" / "setup_openxr_vr.bat"
    if not setup_src.is_file():
        fail(f"Missing setup script: {setup_src}")
    setup_dst = root / "setup_openxr_vr.bat"
    shutil.copy2(setup_src, setup_dst)
    print(f"Copied: {setup_dst}")

    print("\nPhase 1 source patch installed.")


if __name__ == "__main__":
    main()
