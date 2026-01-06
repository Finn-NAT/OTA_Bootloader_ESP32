from __future__ import annotations

import os
import socket
import threading
import time
import json
import queue
from pathlib import Path

from werkzeug.utils import secure_filename

from flask import Flask, jsonify, render_template, request, Response, send_file

app = Flask(__name__)

# Basic hardening: limit upload size (default 50MB). Override via MAX_CONTENT_LENGTH.
app.config["MAX_CONTENT_LENGTH"] = int(os.environ.get("MAX_CONTENT_LENGTH", str(50 * 1024 * 1024)))


# Where to store uploaded firmware images.
# You can override with environment variable: UPLOAD_DIR=... 
UPLOAD_DIR = Path(os.environ.get("UPLOAD_DIR", Path(__file__).parent / "firmware")).resolve()
ALLOWED_EXTENSIONS = {".bin"}


# =========================
# Status / Heartbeat support
# =========================
# ESP32 periodically POSTs to /status (e.g. every 1s). The server considers the
# device offline if it hasn't received a heartbeat within HEARTBEAT_TIMEOUT_S.
#
# Expected payload from ESP32 (nested fields are allowed):
#   {"bootloader_status": 0}  or  {"bootloader_status": 1}
#
# When offline, GET /status and any emitted status will force:
#   status.bootloader_status = -1

HEARTBEAT_TIMEOUT_S = float(os.environ.get("HEARTBEAT_TIMEOUT_S", "5"))

_status_lock = threading.Lock()
_last_seen: float | None = None
_last_status: dict | None = None

# "Update" flag: becomes True when user uploads firmware (start update flow)
# and becomes False when device reports bootloader_status == 1 again.
_update_in_progress: bool = False

# Upload progress text for UI (best-effort).
_upload_phase: str = "idle"  # idle|uploaded|waiting_device|success

# To avoid false 'success' immediately after upload, we require this sequence:
#   upload -> (observe 0) -> then (observe 1) => success
_saw_bootloader_zero_after_upload: bool = False

# Each connected browser gets its own queue.
_subscribers: set[queue.Queue[str]] = set()


def _broadcast(event: str, data: dict) -> None:
    """Push an SSE event to all connected clients."""
    msg = f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"
    dead: list[queue.Queue[str]] = []
    for q in list(_subscribers):
        try:
            q.put_nowait(msg)
        except Exception:
            dead.append(q)
    for q in dead:
        _subscribers.discard(q)


def _set_update_in_progress(v: bool) -> None:
    global _update_in_progress
    if _update_in_progress == v:
        return
    _update_in_progress = v
    _broadcast("update", {"update": _update_in_progress})


def _set_upload_phase(phase: str, detail: str | None = None) -> None:
    global _upload_phase
    if _upload_phase == phase and detail is None:
        return
    _upload_phase = phase
    payload: dict = {"phase": _upload_phase}
    if detail is not None:
        payload["detail"] = detail
    _broadcast("upload", payload)


def _is_online() -> bool:
    # NOTE: keep as small helper; protected by _status_lock where needed
    global _last_seen
    return _last_seen is not None and (time.time() - _last_seen) <= HEARTBEAT_TIMEOUT_S


def _force_offline_status() -> None:
    global _last_status
    if _last_status is None:
        _last_status = {}
    _last_status["bootloader_status"] = -1


def _watchdog() -> None:
    """Marks offline by forcing status.bootloader_status=-1 after timeout."""
    last = False
    while True:
        time.sleep(1.0)
        cur = _is_online()
        if cur != last:
            last = cur
            if not cur:
                with _status_lock:
                    _force_offline_status()

                # Notify UI clients that device went offline.
                _broadcast(
                    "status",
                    {
                        "online": False,
                        "last_seen": _last_seen,
                        "timeout_s": HEARTBEAT_TIMEOUT_S,
                        "status": dict(_last_status or {}),
                    },
                )
                # If device is offline, update flag must be false.
                _set_update_in_progress(False)


threading.Thread(target=_watchdog, daemon=True).start()


def _ensure_upload_dir() -> None:
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


def _delete_existing_bins() -> int:
    """Delete all .bin files in UPLOAD_DIR. Returns number deleted."""
    _ensure_upload_dir()
    deleted = 0
    for p in UPLOAD_DIR.glob("*.bin"):
        try:
            p.unlink()
            deleted += 1
        except OSError:
            # Best-effort; if a file is locked, we'll fail later on save.
            pass
    return deleted


def _latest_bin_path() -> Path | None:
    """Pick the newest .bin inside UPLOAD_DIR (mtime)."""
    _ensure_upload_dir()
    bins = list(UPLOAD_DIR.glob("*.bin"))
    if not bins:
        return None
    bins.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return bins[0]


def _list_bins() -> list[dict]:
    """List all .bin files in UPLOAD_DIR."""
    _ensure_upload_dir()
    out: list[dict] = []
    for p in sorted(UPLOAD_DIR.glob("*.bin"), key=lambda x: x.name.lower()):
        try:
            st = p.stat()
            out.append(
                {
                    "name": p.name,
                    "bytes": st.st_size,
                    "mtime": st.st_mtime,
                    "url": f"/firmware/{p.name}",
                }
            )
        except OSError:
            continue
    return out


def _safe_firmware_path(name: str) -> Path | None:
    safe = secure_filename(name)
    if not safe or Path(safe).suffix.lower() not in ALLOWED_EXTENSIONS:
        return None
    p = (UPLOAD_DIR / safe).resolve()
    if UPLOAD_DIR not in p.parents and p != UPLOAD_DIR:
        return None
    return p


@app.get("/")
def home():
    return render_template(
        "index.html",
        title="Flask Update Firmware via HTTP",
        message=f"Upload firmware folder: {UPLOAD_DIR}",
    )


@app.post("/status")
def post_status():
    """Heartbeat endpoint.

    ESP32 POSTs JSON here periodically.
    Example:
      {"bootloader_status": 1}
    """
    global _last_seen, _last_status
    data = request.get_json(silent=True) or {}

    with _status_lock:
        _last_seen = time.time()
        # Keep status nested (no extra top-level fields).
        _last_status = dict(data)

        # Finish update flow ONLY when device reports bootloader_status == 1 again.
        # (You requested: on upload we force bootloader_status=0; wait until it returns to 1 -> success.)
        # Your rule:
        # - update = true only when user presses Upload
        # - update = false when bootloader_status becomes 0 OR -1
        bs = _last_status.get("bootloader_status")
        try:
            # If device reports it is in bootloader (0) or offline (-1), ensure update flag is false.
            if bs is not None and int(bs) in (0, -1):
                _set_update_in_progress(False)

            # Track bootloader_status transitions for upload success.
            if bs is not None and int(bs) == 0 and _upload_phase == "waiting_device":
                global _saw_bootloader_zero_after_upload
                _saw_bootloader_zero_after_upload = True

            # If device reports bootloader_status == 1 and we were waiting for it,
            # mark upload flow as successful and clear update flag.
            if bs is not None and int(bs) == 1:
                # Only transition to success from the waiting_device phase.
                if _upload_phase == "waiting_device" and _saw_bootloader_zero_after_upload:
                    _set_upload_phase("success", "device reported bootloader_status=1")
                # Clear update flag when device is back to running.
                _set_update_in_progress(False)
        except Exception:
            pass

        snapshot = {
            "online": True,
            "last_seen": _last_seen,
            "timeout_s": HEARTBEAT_TIMEOUT_S,
            "status": dict(_last_status),
        }

    _broadcast("status", snapshot)

    return jsonify({"ok": True})


@app.get("/status")
def get_status():
    global _last_seen, _last_status
    now = time.time()
    with _status_lock:
        online = _last_seen is not None and (now - _last_seen) <= HEARTBEAT_TIMEOUT_S
        payload = dict(_last_status or {})
        if not online:
            payload["bootloader_status"] = -1

        return jsonify(
            {
                "ok": True,
                "online": online,
                "last_seen": _last_seen,
                "timeout_s": HEARTBEAT_TIMEOUT_S,
                "status": payload,
            }
        )


@app.get("/update")
def get_update():
    """ESP32 polling endpoint.

    Keep it compatible with parse_update_firmware_state(): return plain text
    'on'/'off' (NOT JSON).
    """
    return Response("on" if _update_in_progress else "off", mimetype="text/plain")


@app.get("/update_json")
def get_update_json():
    """UI-friendly endpoint (JSON)."""
    return jsonify({"ok": True, "update": _update_in_progress, "phase": _upload_phase})


@app.get("/firmware")
def get_firmware():
    """List all firmware files.

    You said: "tất cả các file có trong folder firmware được up lên server url/firmware".
    So this endpoint returns a JSON listing; you can choose which file to download.
    Download a specific file at: GET /firmware/<filename>.bin
    """
    return jsonify({"ok": True, "files": _list_bins()})


@app.get("/firmware/<path:name>")
def get_firmware_file(name: str):
    """Download a specific firmware file by name."""
    p = _safe_firmware_path(name)
    if p is None:
        return jsonify({"ok": False, "error": "invalid firmware name"}), 400
    if not p.exists():
        return jsonify({"ok": False, "error": "firmware not found"}), 404
    return send_file(p, mimetype="application/octet-stream", as_attachment=False, download_name=p.name)


@app.get("/firmware_info")
def firmware_info():
    """Backward-compatible helper for UI/debug.

    Now that /firmware is a listing, we expose the same information but pointing to /firmware/<name>.
    """
    p = _latest_bin_path()
    if p is None:
        return jsonify({"ok": True, "has_firmware": False, "files": []})

    return jsonify(
        {
            "ok": True,
            "has_firmware": True,
            "name": p.name,
            "bytes": p.stat().st_size,
            "path": str(p),
            "ota_path": f"/firmware/{p.name}",
            "files": _list_bins(),
        }
    )


@app.get("/events")
def events() -> Response:
    """SSE stream for real-time status updates (like led_server.py)."""
    q: queue.Queue[str] = queue.Queue(maxsize=100)
    _subscribers.add(q)

    # Send initial snapshot so UI can render immediately.
    with _status_lock:
        online = _is_online()
        payload = dict(_last_status or {})
        if not online:
            payload["bootloader_status"] = -1
        q.put_nowait(
            "event: snapshot\n"
            f"data: {json.dumps({'online': online, 'last_seen': _last_seen, 'timeout_s': HEARTBEAT_TIMEOUT_S, 'status': payload}, ensure_ascii=False)}\n\n"
        )

    # Also send initial update state.
    q.put_nowait(
        "event: update\n" f"data: {json.dumps({'update': _update_in_progress}, ensure_ascii=False)}\n\n"
    )

    # And initial upload phase.
    q.put_nowait(
        "event: upload\n" f"data: {json.dumps({'phase': _upload_phase}, ensure_ascii=False)}\n\n"
    )

    def gen():
        try:
            while True:
                try:
                    msg = q.get(timeout=15)
                    yield msg
                except queue.Empty:
                    # Keep-alive so proxies don't close the connection
                    yield ": ping\n\n"
        finally:
            _subscribers.discard(q)

    return Response(gen(), mimetype="text/event-stream", headers={"Cache-Control": "no-cache"})


@app.get("/favicon.ico")
def favicon():
    # Avoid noisy 404 logs when browser auto-requests favicon.
    return ("", 204)


@app.post("/upload")
def upload_bin():
    """Upload a new firmware .bin.

    Behavior:
      - deletes all existing *.bin in UPLOAD_DIR
      - saves the newly uploaded file into UPLOAD_DIR
    """
    if "file" not in request.files:
        return jsonify({"ok": False, "error": "missing form field 'file'"}), 400

    f = request.files["file"]
    if not f or not f.filename:
        return jsonify({"ok": False, "error": "no file selected"}), 400

    original_name = f.filename
    safe_name = secure_filename(original_name) or "firmware.bin"
    ext = Path(safe_name).suffix.lower()
    if ext not in ALLOWED_EXTENSIONS:
        return jsonify({"ok": False, "error": "only .bin files are allowed"}), 400

    _ensure_upload_dir()
    deleted = _delete_existing_bins()
    save_path = (UPLOAD_DIR / safe_name).resolve()

    # Prevent path traversal just in case.
    if UPLOAD_DIR not in save_path.parents and save_path != UPLOAD_DIR:
        return jsonify({"ok": False, "error": "invalid filename"}), 400

    try:
        f.save(save_path)
    except Exception as e:
        return jsonify({"ok": False, "error": f"failed to save file: {e}"}), 500

    # Mark update flow started.
    _set_update_in_progress(True)
    _set_upload_phase("uploaded", f"saved {save_path.name}")

    # New upload requires a fresh 0 -> 1 transition to be considered success.
    global _saw_bootloader_zero_after_upload
    _saw_bootloader_zero_after_upload = False

    # Per your requirement: as soon as user presses Upload, bootloader_status should be 0.
    # We "force" the displayed status to 0 so ESP32 will observe it immediately (via /status) even
    # before the real device state changes.
    global _last_status
    with _status_lock:
        if _last_status is None:
            _last_status = {}
        _last_status["bootloader_status"] = 0
        snapshot = {
            "online": _is_online(),
            "last_seen": _last_seen,
            "timeout_s": HEARTBEAT_TIMEOUT_S,
            "status": dict(_last_status),
        }
    _broadcast("status", snapshot)
    _set_upload_phase("waiting_device", "waiting for device bootloader_status=1")

    return jsonify(
        {
            "ok": True,
            "update": _update_in_progress,
            "phase": _upload_phase,
            "deleted_old_bin_files": deleted,
            "saved_as": save_path.name,
            "upload_dir": str(UPLOAD_DIR),
            "bytes": save_path.stat().st_size,
        }
    )


if __name__ == "__main__":
    # Use waitress in production; Flask dev server is OK for local testing.
    host = os.environ.get("HOST", "127.0.0.1").strip()
    port = int(os.environ.get("PORT", "5000"))
    debug = os.environ.get("DEBUG", "1").strip() not in {"0", "false", "False", "no", "NO"}

    # # If you want other devices on the same Wi‑Fi to access this server,
    # # set HOST=0.0.0.0 (bind all network interfaces).
    # if host.lower() in {"all", "0.0.0.0", "0"}:
    #     host = "0.0.0.0"

    # def _lan_ips() -> list[str]:
    #     """Best-effort: enumerate local IPv4 addresses to show the user."""
    #     ips: set[str] = set()
    #     try:
    #         hn = socket.gethostname()
    #         for info in socket.getaddrinfo(hn, None, family=socket.AF_INET):
    #             ip = info[4][0]
    #             if ip and not ip.startswith("127."):
    #                 ips.add(ip)
    #     except Exception:
    #         pass
    #     return sorted(ips)

    # if host == "0.0.0.0":
    #     ips = _lan_ips()
    #     if ips:
    #         print("\nServer is listening on all interfaces. Try these URLs from another device in the same Wi‑Fi:")
    #         for ip in ips:
    #             print(f"  http://{ip}:{port}/")
    #     else:
    #         print("\nServer is listening on all interfaces. Use your PC's Wi‑Fi IPv4 to access it from other devices.")

    app.run(host=host, port=port, debug=debug)
