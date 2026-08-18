from pathlib import Path

from app.memory import ConversationMemory


def test_memory_persists_and_trims(tmp_path: Path) -> None:
    path = tmp_path / "memory.json"
    memory = ConversationMemory(path, max_messages=2)
    memory.add("user", "one")
    memory.add("assistant", "two")
    memory.add("user", "three")

    reloaded = ConversationMemory(path, max_messages=2)
    messages = reloaded.as_chat_messages()
    assert [item["content"] for item in messages] == ["two", "three"]


def test_clear(tmp_path: Path) -> None:
    path = tmp_path / "memory.json"
    memory = ConversationMemory(path)
    memory.add("user", "hello")
    memory.clear()
    assert memory.as_chat_messages() == []
