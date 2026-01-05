from __future__ import annotations

import os
import json
import queue
from dataclasses import dataclass
import threading
import time
from typing import Iterator

from flask import Flask, jsonify, render_template, request, Response


@dataclass
class LedState:
    on: bool = False
    last_count: int | None = None
    last_seen: float | None = None
    last_status: dict | None = None


def create_app() -> Flask:
    app = Flask(__name__)
    state = LedState(on=False)

    # Heartbeat rules: if no heartbeat within this window, UI should show offline.
    HEARTBEAT_TIMEOUT_S = 5.0

    def _is_online() -> bool:
        return state.last_seen is not None and (time.time() - state.last_seen) <= HEARTBEAT_TIMEOUT_S

    def _watchdog() -> None:
        """Periodically checks heartbeat age and broadcasts online/offline transitions."""
        last = _is_online()
        while True:
            time.sleep(1.0)
            cur = _is_online()
            if cur != last:
                last = cur
                if not cur:
                    # When offline, force nested status.bootloader_status = -1
                    if state.last_status is None:
                        state.last_status = {}
                    state.last_status["bootloader_status"] = -1
                _broadcast(
                    "status",
                    {
                        "online": cur,
                        "last_seen": state.last_seen,
                        "timeout_s": HEARTBEAT_TIMEOUT_S,
                        "status": state.last_status,
                    },
                )

    threading.Thread(target=_watchdog, daemon=True).start()

    # Each connected browser gets its own queue.
    subscribers: set[queue.Queue[str]] = set()

    def _broadcast(event: str, data: dict) -> None:
        """Push an SSE event to all connected clients."""
        msg = f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"
        dead: list[queue.Queue[str]] = []
        for q in list(subscribers):
            try:
                q.put_nowait(msg)
            except Exception:
                dead.append(q)
        for q in dead:
            subscribers.discard(q)

    @app.get("/led")
    def get_led() -> Response:
        # ESP32 expects plain text: on/off
        return Response("on" if state.on else "off", mimetype="text/plain")

    @app.post("/led")
    def set_led():
        # Accept JSON: {"state": "on"|"off"} or {"on": true|false}
        data = request.get_json(silent=True) or {}

        value = None
        if "state" in data:
            value = str(data["state"]).strip().lower()
            if value in {"on", "1", "true"}:
                state.on = True
            elif value in {"off", "0", "false"}:
                state.on = False
            else:
                return jsonify({"ok": False, "error": "state must be on/off"}), 400
        elif "on" in data:
            state.on = bool(data["on"])
        else:
            return jsonify({"ok": False, "error": "missing json key: state or on"}), 400

        _broadcast("led", {"on": state.on})
        return jsonify({"ok": True, "on": state.on})

    @app.get("/")
    def ui():
        return render_template("led.html")


    @app.post("/status")
    def post_status():
        """Heartbeat endpoint.

        ESP32 can POST JSON like:
          {"status":"ok"} or {"status":"running","uptime_ms":1234}

        We'll store last_seen and broadcast to SSE clients.
        """
        data = request.get_json(silent=True) or {}
        state.last_seen = time.time()
        # Keep the payload under nested 'status' only.
        # ESP32 should send: {"bootloader_status": 0/1}
        state.last_status = data

        _broadcast(
            "status",
            {
                "online": True,
                "last_seen": state.last_seen,
                "status": data,
            },
        )
        return jsonify({"ok": True})

    @app.get("/status")
    def get_status():
        now = time.time()
        online = state.last_seen is not None and (now - state.last_seen) <= HEARTBEAT_TIMEOUT_S

        # When offline, force nested status.bootloader_status = -1.
        status_payload = dict(state.last_status or {})
        if not online:
            status_payload["bootloader_status"] = -1
        return jsonify(
            {
                "ok": True,
                "online": online,
                "last_seen": state.last_seen,
                "timeout_s": HEARTBEAT_TIMEOUT_S,
                "status": status_payload,
            }
        )


    @app.post("/count")
    def count():
        # Accept JSON: {"count": 123}
        data = request.get_json(silent=True) or {}
        if "count" not in data:
            return jsonify({"ok": False, "error": "missing json key: count"}), 400
        try:
            state.last_count = int(data["count"])
        except Exception:
            return jsonify({"ok": False, "error": "count must be an integer"}), 400

        # Log to console for quick debugging.
        print(f"[COUNT] {state.last_count}")
        _broadcast("count", {"count": state.last_count})
        return jsonify({"ok": True, "count": state.last_count})

    @app.get("/count")
    def get_count():
        return jsonify({"ok": True, "last_count": state.last_count})

    @app.get("/events")
    def events() -> Response:
        """SSE stream for real-time UI updates."""
        q: queue.Queue[str] = queue.Queue(maxsize=100)
        subscribers.add(q)

        # Send initial snapshot so UI can render immediately.
        q.put_nowait(
            "event: snapshot\n"
            f"data: {json.dumps({'on': state.on, 'last_count': state.last_count, 'online': _is_online(), 'last_seen': state.last_seen, 'timeout_s': HEARTBEAT_TIMEOUT_S, 'status': (dict(state.last_status or {}) if _is_online() else dict({**(state.last_status or {}), 'bootloader_status': -1}))}, ensure_ascii=False)}\n\n"
        )

        def gen() -> Iterator[str]:
            try:
                while True:
                    try:
                        msg = q.get(timeout=15)
                        yield msg
                    except queue.Empty:
                        # Keep-alive so proxies don't close the connection
                        yield ": ping\n\n"
            finally:
                subscribers.discard(q)

        return Response(gen(), mimetype="text/event-stream", headers={"Cache-Control": "no-cache"})

    return app


if __name__ == "__main__":
    host = os.environ.get("HOST", "0.0.0.0").strip()
    port = int(os.environ.get("PORT", "8000"))
    debug = os.environ.get("DEBUG", "1").strip() not in {"0", "false", "False", "no", "NO"}

    app = create_app()
    app.run(host=host, port=port, debug=debug)
