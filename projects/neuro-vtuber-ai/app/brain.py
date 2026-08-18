from __future__ import annotations

import requests

from .config import Settings
from .memory import ConversationMemory
from .personality import build_system_prompt


class OllamaBrain:
    def __init__(self, settings: Settings, memory: ConversationMemory) -> None:
        self.settings = settings
        self.memory = memory

    def chat(self, user_text: str) -> str:
        messages = [
            {"role": "system", "content": build_system_prompt(self.settings.character_name)},
            *self.memory.as_chat_messages(),
            {"role": "user", "content": user_text},
        ]

        payload = {
            "model": self.settings.ollama_model,
            "messages": messages,
            "stream": False,
            "options": {"temperature": 0.9, "num_predict": 180},
        }

        try:
            response = requests.post(
                f"{self.settings.ollama_url.rstrip('/')}/api/chat",
                json=payload,
                timeout=120,
            )
            response.raise_for_status()
            data = response.json()
        except requests.RequestException as exc:
            raise RuntimeError(
                "Could not reach Ollama. Start Ollama and make sure the configured model is installed."
            ) from exc

        content = data.get("message", {}).get("content", "")
        if not isinstance(content, str) or not content.strip():
            raise RuntimeError("Ollama returned an empty response.")
        return content.strip()
