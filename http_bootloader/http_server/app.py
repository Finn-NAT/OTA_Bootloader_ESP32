from __future__ import annotations

import os
import socket
from pathlib import Path

from werkzeug.utils import secure_filename

from flask import Flask, jsonify, render_template, request

app = Flask(__name__)

# Basic hardening: limit upload size (default 50MB). Override via MAX_CONTENT_LENGTH.
app.config["MAX_CONTENT_LENGTH"] = int(os.environ.get("MAX_CONTENT_LENGTH", str(50 * 1024 * 1024)))


# Where to store uploaded firmware images.
# You can override with environment variable: UPLOAD_DIR=... 
UPLOAD_DIR = Path(os.environ.get("UPLOAD_DIR", Path(__file__).parent / "firmware")).resolve()
ALLOWED_EXTENSIONS = {".bin"}


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


@app.get("/")
def home():
    return render_template(
        "index.html",
        title="Flask Update Firmware via HTTP",
        message=f"Upload firmware folder: {UPLOAD_DIR}",
    )


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

    return jsonify(
        {
            "ok": True,
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
