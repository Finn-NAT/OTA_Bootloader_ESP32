from __future__ import annotations

import os
import json
import queue
from dataclasses import dataclass
from typing import Iterator

from flask import Flask, jsonify, render_template, request, Response

from flask import Flask, jsonify, render_template, request, Response


@dataclass
class LedState:
    on: bool = False
    last_count: int | None = None


def create_app() -> Flask:
    app = Flask(__name__)
    state = LedState(on=False)

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
            f"data: {json.dumps({'on': state.on, 'last_count': state.last_count}, ensure_ascii=False)}\n\n"
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
