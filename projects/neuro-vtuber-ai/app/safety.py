from __future__ import annotations


def clean_spoken_output(text: str, max_chars: int = 500) -> str:
    cleaned = " ".join(text.replace("\x00", "").split())
    if len(cleaned) > max_chars:
        cleaned = cleaned[: max_chars - 1].rstrip() + "…"
    return cleaned
