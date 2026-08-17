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


def insert_after_once(text: str, marker: str, addition: str, label: str) -> str:
    if addition.strip() in text:
        print(f"Already patched: {label}")
        return text
    pos = text.find(marker)
    if pos < 0:
        fail(f"Could not find patch marker for {label}: {marker!r}")
    end = pos + len(marker)
    print(f"Patched: {label}")
    return text[:end] + addition + text[end:]


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
        target = modules / name
        shutil.copy2(source, target)
        print(f"Copied: {target}")

    loader_text = loader.read_text(encoding="utf-8")
    newline = "\r\n" if "\r\n" in loader_text else "\n"
    loader_text = insert_after_once(
        loader_text,
        '#include "Modules/Node.hpp"' + newline,
        '#include "Modules/OpenXR.hpp"' + newline,
        "Loader include",
    )
    loader_text = insert_after_once(
        loader_text,
        "\tRegister(new Scheduler());" + newline,
        "\tRegister(new OpenXR());" + newline,
        "OpenXR component registration",
    )
    loader.write_text(loader_text, encoding="utf-8", newline="")

    premake_text = premake.read_text(encoding="utf-8")
    newline = "\r\n" if "\r\n" in premake_text else "\n"
    link_block = (
        '\tlibdirs { "./lib/openxr/win32" }' + newline
        + '\tlinks { "d3d11", "dxgi", "openxr_loader" }' + newline
    )
    premake_text = insert_after_once(
        premake_text,
        "\tdependencies.imports()" + newline,
        link_block,
        "OpenXR/D3D11 linker settings",
    )
    premake.write_text(premake_text, encoding="utf-8", newline="")

    setup_src = PACKAGE / "tools" / "setup_openxr_vr.bat"
    setup_dst = root / "setup_openxr_vr.bat"
    shutil.copy2(setup_src, setup_dst)
    print(f"Copied: {setup_dst}")

    print("\nPhase 1 source patch installed.")
    print("Next commands from the IW4x repo root:")
    print("  setup_openxr_vr.bat")
    print("  generate.bat")
    print(r"Then build build\iw4x.sln -> Win32 / Release.")


if __name__ == "__main__":
    main()
