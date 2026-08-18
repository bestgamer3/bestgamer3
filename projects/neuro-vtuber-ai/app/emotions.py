from __future__ import annotations

import re


EMOTION_KEYWORDS: dict[str, tuple[str, ...]] = {
    "angry": ("angry", "annoyed", "mad", "rage", "seriously?!", "unbelievable"),
    "shocked": ("what?!", "no way", "wait, what", "oh my", "shocked", "wow!"),
    "sad": ("sad", "sorry", "unfortunate", "aw...", "rip", "tragic"),
    "smug": ("obviously", "too easy", "skill issue", "as expected", "clearly"),
    "happy": ("haha", "lol", "nice", "awesome", "yay", "love", "great", ":)"),
}


def detect_emotion(text: str) -> str:
    lowered = re.sub(r"\s+", " ", text.lower()).strip()
    for emotion, keywords in EMOTION_KEYWORDS.items():
        if any(keyword in lowered for keyword in keywords):
            return emotion
    if text.count("!") >= 2:
        return "shocked"
    return "neutral"
