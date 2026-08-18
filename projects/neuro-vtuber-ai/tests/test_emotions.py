from app.emotions import detect_emotion


def test_happy_keyword() -> None:
    assert detect_emotion("Haha, that was awesome!") == "happy"


def test_shocked_punctuation() -> None:
    assert detect_emotion("You did WHAT!!") == "shocked"


def test_neutral_fallback() -> None:
    assert detect_emotion("I am thinking about that.") == "neutral"
