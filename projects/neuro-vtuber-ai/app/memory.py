from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class ConversationMemory:
    def __init__(self, path: Path, max_messages: int = 24) -> None:
        self.path = path
        self.max_messages = max_messages
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.messages: list[dict[str, Any]] = self._load()

    def _load(self) -> list[dict[str, Any]]:
        if not self.path.exists():
            return []
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if isinstance(data, list):
                return [item for item in data if isinstance(item, dict)]
        except (json.JSONDecodeError, OSError):
            pass
        return []

    def _save(self) -> None:
        trimmed = self.messages[-self.max_messages :]
        self.messages = trimmed
        self.path.write_text(json.dumps(trimmed, indent=2, ensure_ascii=False), encoding="utf-8")

    def add(self, role: str, content: str) -> None:
        self.messages.append({
            "role": role,
            "content": content,
            "timestamp": datetime.now(timezone.utc).isoformat(),
        })
        self._save()

    def add_exchange(self, user_text: str, assistant_text: str) -> None:
        self.add("user", user_text)
        self.add("assistant", assistant_text)

    def as_chat_messages(self) -> list[dict[str, str]]:
        return [
            {"role": str(item.get("role", "user")), "content": str(item.get("content", ""))}
            for item in self.messages[-self.max_messages :]
            if item.get("content")
        ]

    def clear(self) -> None:
        self.messages = []
        self._save()
