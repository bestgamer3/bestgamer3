from __future__ import annotations


class TextToSpeech:
    def __init__(self, enabled: bool = True, rate: int = 185, volume: float = 1.0) -> None:
        self.enabled = enabled
        self.rate = rate
        self.volume = max(0.0, min(1.0, volume))
        self._engine = None

    def _get_engine(self):
        if self._engine is None:
            import pyttsx3

            self._engine = pyttsx3.init()
            self._engine.setProperty("rate", self.rate)
            self._engine.setProperty("volume", self.volume)
        return self._engine

    def speak(self, text: str) -> None:
        if not self.enabled or not text:
            return
        try:
            engine = self._get_engine()
            engine.say(text)
            engine.runAndWait()
        except Exception as exc:
            print(f"[TTS warning] {exc}")
