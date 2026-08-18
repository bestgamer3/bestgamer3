from __future__ import annotations

from .brain import OllamaBrain
from .config import Settings
from .emotions import detect_emotion
from .memory import ConversationMemory
from .safety import clean_spoken_output
from .tts import TextToSpeech
from .vtube_studio import VTubeStudioClient


def print_help() -> None:
    print("Commands:")
    print("  /help   Show commands")
    print("  /clear  Clear saved conversation memory")
    print("  /quit   Exit")


def run() -> None:
    settings = Settings()
    memory = ConversationMemory(settings.memory_file, settings.memory_max_messages)
    brain = OllamaBrain(settings, memory)
    tts = TextToSpeech(settings.tts_enabled, settings.tts_rate, settings.tts_volume)

    vts = VTubeStudioClient(
        settings.vts_url,
        settings.vts_token_file,
        settings.vts_plugin_name,
        settings.vts_plugin_developer,
    )

    print(f"\n=== {settings.character_name} AI VTuber ===")
    print(f"Brain: Ollama / {settings.ollama_model}")
    print(f"TTS: {'ON' if settings.tts_enabled else 'OFF'}")
    print(f"VTube Studio: {'ON' if settings.vts_enabled else 'OFF'}")
    print("Type /help for commands.\n")

    while True:
        try:
            user_text = input("You > ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nBye!")
            break

        if not user_text:
            continue
        if user_text.lower() == "/quit":
            print("Bye!")
            break
        if user_text.lower() == "/help":
            print_help()
            continue
        if user_text.lower() == "/clear":
            memory.clear()
            print("Memory cleared.")
            continue

        try:
            raw_response = brain.chat(user_text)
            response = clean_spoken_output(raw_response, settings.max_output_chars)
        except RuntimeError as exc:
            print(f"[AI error] {exc}\n")
            continue

        emotion = detect_emotion(response)
        print(f"{settings.character_name} [{emotion}] > {response}\n")
        memory.add_exchange(user_text, response)

        if settings.vts_enabled:
            hotkey = settings.hotkey_map().get(emotion, "")
            if hotkey:
                try:
                    vts.trigger_hotkey_sync(hotkey)
                except Exception as exc:
                    print(f"[VTube Studio warning] {exc}")

        tts.speak(response)


if __name__ == "__main__":
    run()
