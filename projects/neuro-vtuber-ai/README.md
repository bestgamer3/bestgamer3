# Nova AI VTuber

A starter **Neuro-sama-style AI VTuber architecture** written in Python. It is intentionally an original character/project rather than a copy of Neuro-sama.

Version 1 includes:

- local AI conversation through Ollama
- editable VTuber personality
- persistent short-term conversation memory
- offline text-to-speech with `pyttsx3`
- simple emotion detection
- optional VTube Studio WebSocket integration
- VTube Studio hotkey triggering for expressions
- Windows setup/run scripts
- basic automated tests

## Architecture

```text
You / future Twitch chat
        |
        v
   Python controller
        |
        +----> Ollama LLM ----> spoken reply
        |                         |
        |                         +----> memory.json
        |                         +----> pyttsx3 voice
        |                         +----> emotion detector
        |                                   |
        |                                   v
        +----------------------------> VTube Studio
```

## Requirements

- Windows 10/11 recommended for the starter scripts
- Python 3.11+
- Ollama
- optional: VTube Studio + a Live2D model

## Install

Run `setup.bat`, then install/pull an Ollama model:

```powershell
ollama pull gemma3
```

Make sure Ollama is running, then run `run.bat`.

## Commands

- `/help` - show commands
- `/clear` - clear conversation memory
- `/quit` - exit

## Change personality

Edit `app/personality.py`.

## Enable VTube Studio

1. In VTube Studio enable **Allow Plugin API access**.
2. Create hotkeys named `Happy`, `Sad`, `Angry`, `Shocked`, `Smug`, and `Neutral`, or choose your own names.
3. Set `VTS_ENABLED=true` in `.env`.
4. If your hotkey names differ, edit the `VTS_HOTKEY_*` values.

The first connection asks for plugin permission and stores the token locally in `.vts_token`.

## Roadmap

V2: Twitch chat, message queue, viewer context, moderation.

V3: OBS WebSocket, subtitles, sounds and scene changes.

V4: game-state adapters, vision/telemetry, planning and game controls.

This project does not contain Neuro-sama's code, voice, model, assets, prompts, or proprietary systems.
