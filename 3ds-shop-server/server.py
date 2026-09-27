import os
import sys
import struct

print("Loading dependencies...")

try:
    from flask import Flask, jsonify, request, send_file
    print("Flask loaded successfully.")
except ImportError:
    print("\n[ERROR] Flask is not installed in this Python environment!")
    print("Run: pip install flask")
    sys.exit(1)

app = Flask(__name__)

# Target directory
GAMES_DIR = "C:/path/to/your/games"

TID_CACHE = {}

def extract_cia_title_id(filepath):
    if filepath in TID_CACHE:
        return TID_CACHE[filepath]

    tid_result = "0"
    try:
        with open(filepath, "rb") as f:
            header_chunk = f.read(0x20000)

        if len(header_chunk) >= 0x20:
            u32s = struct.unpack_from("<5I", header_chunk, 0)
            header_size = u32s[0]
            cert_size   = u32s[2]
            ticket_size = u32s[3]
            tmd_size    = u32s[4]

            align = lambda x: (x + 0x3F) & ~0x3F
            aligned_header = align(header_size)
            aligned_cert   = align(cert_size)
            aligned_ticket = align(ticket_size)

            # Check Ticket (Offset: aligned_header + aligned_cert + 0x1DC)
            if ticket_size > 0:
                ticket_offset = aligned_header + aligned_cert
                target = ticket_offset + 0x1DC
                if len(header_chunk) >= target + 8:
                    tid_bytes = header_chunk[target : target + 8]
                    tid_hex = f"{struct.unpack('>Q', tid_bytes)[0]:016X}"
                    if tid_hex.startswith("0004"):
                        tid_result = tid_hex

            # Check TMD (Offset: aligned_header + aligned_cert + aligned_ticket + 0x18C)
            if tid_result == "0" and tmd_size > 0:
                tmd_offset = aligned_header + aligned_cert + aligned_ticket
                target = tmd_offset + 0x18C
                if len(header_chunk) >= target + 8:
                    tid_bytes = header_chunk[target : target + 8]
                    tid_hex = f"{struct.unpack('>Q', tid_bytes)[0]:016X}"
                    if tid_hex.startswith("0004"):
                        tid_result = tid_hex

    except Exception as e:
        print(f"[!] Header error on {os.path.basename(filepath)}: {e}")

    TID_CACHE[filepath] = tid_result
    return tid_result

@app.route("/api/browse")
def browse():
    rel_path = request.args.get("path", "").strip("/\\")
    full_path = os.path.normpath(os.path.join(GAMES_DIR, rel_path))

    if not os.path.abspath(full_path).startswith(os.path.abspath(GAMES_DIR)):
        return jsonify({"items": [], "error": "Invalid path"}), 400

    if not os.path.isdir(full_path):
        return jsonify({"items": []}), 200

    items = []
    try:
        entries = sorted(os.scandir(full_path), key=lambda e: (not e.is_dir(), e.name.lower()))
        for entry in entries:
            if entry.name.startswith("."):
                continue

            item_rel = os.path.relpath(entry.path, GAMES_DIR).replace("\\", "/")

            if entry.is_dir():
                items.append({
                    "name": entry.name,
                    "type": "directory",
                    "path": item_rel,
                    "size_mb": 0.0,
                    "title_id": "0"
                })
            elif entry.name.lower().endswith(".cia"):
                try:
                    size_mb = round(entry.stat().st_size / (1024 * 1024), 2)
                except OSError:
                    size_mb = 0.0

                tid = extract_cia_title_id(entry.path)

                items.append({
                    "name": entry.name,
                    "type": "file",
                    "path": item_rel,
                    "size_mb": size_mb,
                    "title_id": tid
                })
    except Exception as e:
        print(f"[!] Scan error: {e}")
        return jsonify({"items": [], "error": str(e)}), 500

    return jsonify({"items": items}), 200

@app.route("/download")
def download():
    rel_path = request.args.get("path", "").strip("/\\")
    full_path = os.path.normpath(os.path.join(GAMES_DIR, rel_path))

    if not os.path.abspath(full_path).startswith(os.path.abspath(GAMES_DIR)):
        return "Forbidden", 403

    if not os.path.isfile(full_path):
        return "File Not Found", 404

    return send_file(full_path, as_attachment=True, conditional=True)

if __name__ == "__main__":
    if not os.path.isdir(GAMES_DIR):
        print(f"\n[WARNING] Directory does not exist: {GAMES_DIR}")
        print("Please verify the folder path!")
    else:
        print(f"Games folder verified: {GAMES_DIR}")

    print("\nStarting local HTTP server on port 5000...")
    app.run(host="0.0.0.0", port=5000, threaded=True)
