from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv


ROOT = Path(__file__).resolve().parents[1]
load_dotenv(ROOT / ".env")


def _env_bool(name: str, default: bool = False) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class Settings:
    character_name: str = os.getenv("CHARACTER_NAME", "Nova")
    ollama_url: str = os.getenv("OLLAMA_URL", "http://127.0.0.1:11434")
    ollama_model: str = os.getenv("OLLAMA_MODEL", "gemma3")
    memory_file: Path = ROOT / os.getenv("MEMORY_FILE", "data/memory.json")
    memory_max_messages: int = int(os.getenv("MEMORY_MAX_MESSAGES", "24"))
    tts_enabled: bool = _env_bool("TTS_ENABLED", True)
    tts_rate: int = int(os.getenv("TTS_RATE", "185"))
    tts_volume: float = float(os.getenv("TTS_VOLUME", "1.0"))
    vts_enabled: bool = _env_bool("VTS_ENABLED", False)
    vts_url: str = os.getenv("VTS_URL", "ws://127.0.0.1:8001")
    vts_token_file: Path = ROOT / os.getenv("VTS_TOKEN_FILE", ".vts_token")
    vts_plugin_name: str = os.getenv("VTS_PLUGIN_NAME", "Nova AI VTuber")
    vts_plugin_developer: str = os.getenv("VTS_PLUGIN_DEVELOPER", "bestgamer3")
    max_output_chars: int = int(os.getenv("MAX_OUTPUT_CHARS", "500"))

    def hotkey_map(self) -> dict[str, str]:
        return {
            "happy": os.getenv("VTS_HOTKEY_HAPPY", "Happy"),
            "sad": os.getenv("VTS_HOTKEY_SAD", "Sad"),
            "angry": os.getenv("VTS_HOTKEY_ANGRY", "Angry"),
            "shocked": os.getenv("VTS_HOTKEY_SHOCKED", "Shocked"),
            "smug": os.getenv("VTS_HOTKEY_SMUG", "Smug"),
            "neutral": os.getenv("VTS_HOTKEY_NEUTRAL", "Neutral"),
        }
