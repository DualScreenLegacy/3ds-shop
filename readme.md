# 3DS Local Shop

A lightweight local network content delivery and installation system for Nintendo 3DS consoles. It dynamically extracts Title IDs on a host Python server, streams `.cia` files over HTTP using `libcurl`, and commits them directly to SD card storage via `libctru` Application Manager (`AM`) services, bypassing manual SD card file transfers.

---

## Features

- **Direct Network Streaming:** Streams `.cia` payloads straight into system storage without requiring intermediary staging space on the SD card.
- **Dynamic Title ID Extraction:** Host server reads CTR-CIA headers (TMD and Ticket chunks) on the fly to accurately extract 64-bit Title IDs.
- **Buffered 128 KB Aligned Writes:** Implements page-aligned memory buffers for `FSFILE_Write` IPC calls, preventing cluster allocation faults and media full errors.
- **Automatic Collision & Overwrite Handling:** Automatically cleans lingering staging files (`AM_DeletePendingTitle`) and registered titles prior to installation.
- **Real-Time HUD:** Features a dynamic ANSI progress bar, percentage tracker, downloaded megabytes counter, and live network transfer throughput (KB/s or MB/s).
- **Manual Title Management:** Single-button title and ticket purging directly from the browser file list using the **[X]** key.

---


## Screenshots

<img width="8640" height="4320" alt="_29 09 26_04 15 40 772" src="https://github.com/user-attachments/assets/b003949c-009b-4935-8413-00a0579c7251" />
<img width="8640" height="4320" alt="_29 09 26_04 15 51 42" src="https://github.com/user-attachments/assets/135f372b-5f62-47bd-a007-9c92aa79737a" />
<img width="1513" height="1157" alt="_29 09 26_04 19 00 644" src="https://github.com/user-attachments/assets/c73acbb9-0915-4d03-ad9b-dd814a51effd" />


---


## Prerequisites

### 1. Host Machine (Server)
* **Python 3.8+**
* **Flask:**
  ```bash
  pip install flask
  ```

### 2. Development Environment (Client Build)
* **devkitPro & devkitARM** toolchain with MSYS2.
* Required 3DS development packages installed inside the devkitPro MSYS2 shell:
  ```bash
  pacman -S 3ds-curl 3ds-jansson 3ds-zlib 3ds-bzip2 3ds-mbedtls
  ```

### 3. Nintendo 3DS System
* Custom Firmware (Luma3DS).
* **Title Takeover configured:** To acquire necessary `am:u` / `am:app` permissions for title installation, the Homebrew Launcher **must** be launched via Title Takeover (e.g., using **Download Play**) rather than standard Applet Mode.

---

## Component Setup

### 1. Host Server (`server.py`)
Create a file named `server.py` on your host machine:

```python
import os
import struct
from flask import Flask, jsonify, request, send_file

app = Flask(__name__)

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

            if ticket_size > 0:
                ticket_offset = aligned_header + aligned_cert
                target = ticket_offset + 0x1DC
                if len(header_chunk) >= target + 8:
                    tid_bytes = header_chunk[target : target + 8]
                    tid_hex = f"{struct.unpack('>Q', tid_bytes)[0]:016X}"
                    if tid_hex.startswith("0004"):
                        tid_result = tid_hex

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
    app.run(host="0.0.0.0", port=5000, threaded=True)
```

### 2. Client Application (`source/main.c`)
Ensure your `SERVER_URL` points to your host PC's local IP address. Build using standard `devkitPro` makefiles.

---

## Building the Client

1. Open the **devkitPro MSYS2** terminal and navigate to your project workspace:
   ```bash
   cd /c/path/to/3ds-shop
   ```
2. Compile the binary:
   ```bash
   make clean
   make
   ```
3. Transfer the compiled `3ds-shop.3dsx` to your 3DS SD card:
   ```text
   sdmc:/3ds/3ds-shop/3ds-shop.3dsx
   ```

---

## Usage Instructions

1. Start the server on your host machine:
   ```cmd
   python server.py
   ```
2. Connect your Nintendo 3DS to the same local Wi-Fi network.
3. Launch **Download Play** on the 3DS.
4. Open the Rosalina Menu (`L + D-Pad Down + Select`) -> **Miscellaneous options** -> **Switch the hb. title to the current app.**
5. Exit Rosalina, close Download Play, and relaunch Download Play to access the Homebrew Launcher with elevated privileges.
6. Launch **3DS Local Shop**.
7. **Controls:**
   - **D-Pad Up / Down:** Navigate directories and game selections.
   - **[A]:** Enter directory / Download & Install selected `.cia`.
   - **[B]:** Navigate back up one directory level.
   - **[X]:** Delete installed title, tickets, and staging remnants.
   - **[START]:** Exit client.

---

## Troubleshooting & Error Reference

### `StartCiaInstall Failed: 0xD8E007F7`
* **Cause:** The title is already registered or an uncommitted temporary staging file remains from an interrupted transfer[cite: 1, 2, 3].
* **Resolution:** Highlight the file and press **[X]** to invoke `AM_DeletePendingTitle`, or reboot the 3DS console to clear open file descriptor locks.

### `StartCiaInstall Failed: 0xD900182F`
* **Cause:** Permission mismatch or invalid authorization level in `am:u` / `am:app`[cite: 4].
* **Resolution:** Ensure the Homebrew Launcher is launched via **Title Takeover** (e.g., Download Play) rather than applet mode.

### `Write Error: 0xE0E083F2`
* **Cause:** Storage media full or unaligned IPC block writes exhausting cluster allocation maps.
* **Resolution:** Addressed in client builds via 128 KB page-aligned heap buffering (`memalign`). Ensure your SD card has adequate free blocks and verify cluster formatting if persistent.

### `Directory is empty or offline`
* **Cause:** HTTP communication failure or network timeout.
* **Resolution:** Verify local network connectivity, confirm your PC's firewall allows incoming traffic on port `5000`, and test the browse endpoint via `curl http://<HOST_IP>:5000/api/browse?path=`.
