from __future__ import annotations

import asyncio
import json
import uuid
from pathlib import Path
from typing import Any

import websockets


class VTubeStudioClient:
    API_NAME = "VTubeStudioPublicAPI"
    API_VERSION = "1.0"

    def __init__(self, url: str, token_file: Path, plugin_name: str, plugin_developer: str) -> None:
        self.url = url
        self.token_file = token_file
        self.plugin_name = plugin_name
        self.plugin_developer = plugin_developer

    def _request(self, message_type: str, data: dict[str, Any] | None = None) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "apiName": self.API_NAME,
            "apiVersion": self.API_VERSION,
            "requestID": uuid.uuid4().hex[:32],
            "messageType": message_type,
        }
        if data is not None:
            payload["data"] = data
        return payload

    async def _send(self, websocket, payload: dict[str, Any]) -> dict[str, Any]:
        await websocket.send(json.dumps(payload))
        response = json.loads(await websocket.recv())
        if response.get("messageType") == "APIError":
            message = response.get("data", {}).get("message", "Unknown VTube Studio API error")
            raise RuntimeError(message)
        return response

    async def _get_or_create_token(self, websocket) -> str:
        if self.token_file.exists():
            token = self.token_file.read_text(encoding="utf-8").strip()
            if token:
                return token

        response = await self._send(
            websocket,
            self._request("AuthenticationTokenRequest", {
                "pluginName": self.plugin_name,
                "pluginDeveloper": self.plugin_developer,
            }),
        )
        token = str(response.get("data", {}).get("authenticationToken", "")).strip()
        if not token:
            raise RuntimeError("VTube Studio did not return an authentication token.")
        self.token_file.write_text(token, encoding="utf-8")
        return token

    async def _authenticate(self, websocket, token: str) -> None:
        response = await self._send(
            websocket,
            self._request("AuthenticationRequest", {
                "pluginName": self.plugin_name,
                "pluginDeveloper": self.plugin_developer,
                "authenticationToken": token,
            }),
        )
        if not response.get("data", {}).get("authenticated"):
            raise RuntimeError(response.get("data", {}).get("reason", "VTube Studio authentication failed."))

    async def trigger_hotkey(self, hotkey_name: str) -> None:
        if not hotkey_name:
            return
        async with websockets.connect(self.url, open_timeout=5) as websocket:
            token = await self._get_or_create_token(websocket)
            try:
                await self._authenticate(websocket, token)
            except RuntimeError:
                if self.token_file.exists():
                    self.token_file.unlink()
                token = await self._get_or_create_token(websocket)
                await self._authenticate(websocket, token)

            await self._send(
                websocket,
                self._request("HotkeyTriggerRequest", {"hotkeyID": hotkey_name}),
            )

    def trigger_hotkey_sync(self, hotkey_name: str) -> None:
        asyncio.run(self.trigger_hotkey(hotkey_name))
