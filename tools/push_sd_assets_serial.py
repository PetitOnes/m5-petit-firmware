#!/usr/bin/env python3
"""M5 Petit に USBシリアル経由で顔画像・効果音を送信する / Push face images & sounds over USB serial.

WiFi不要。M5をPCにUSBで繋いだまま(書き込み直後など)、シリアル(例: /dev/ttyACM0)
経由でSDカードにアセットを書き込む。プロトコルは firmware/m5_petit/serial_push.h 参照。

Usage:
    python3 tools/push_sd_assets_serial.py /dev/ttyACM0
    python3 tools/push_sd_assets_serial.py /dev/ttyACM0 --assets ./my_assets_dir
    python3 tools/push_sd_assets_serial.py /dev/ttyACM0 --dry-run

前提 / Requirements:
  - M5側にSDカードが挿さっていること
  - pyserial (`uv pip install pyserial` / `pip install pyserial`)
"""

import argparse
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ZIP = REPO_ROOT / "sd.zip"
BAUD = 115200
CHUNK_SIZE = 256  # must match SERIAL_PUSH_CHUNK in firmware/m5_petit/serial_push.h


def drain_stale(ser: serial.Serial, quiet_ms: int = 300) -> None:
    """A previous run that was killed mid-transfer can leave replies queued in
    the OS receive buffer; reset_input_buffer() alone can race with bytes still
    arriving over USB. Actively read until nothing has arrived for quiet_ms.
    前回異常終了した際の応答がバッファに残っていることがある。reset_input_buffer()
    だけだとUSB到着との競合で取りこぼすため、一定時間何も来なくなるまで読み捨てる。"""
    old_timeout = ser.timeout
    ser.timeout = 0.05
    quiet_since = time.monotonic()
    while time.monotonic() - quiet_since < quiet_ms / 1000:
        chunk = ser.read(4096)
        if chunk:
            quiet_since = time.monotonic()
    ser.timeout = old_timeout


def push_one(ser: serial.Serial, remote_path: str, data: bytes, timeout: int = 60) -> bool:
    ser.write(f"PUSH {remote_path} {len(data)}\n".encode())
    ser.timeout = timeout
    reply = ser.readline().decode(errors="replace").strip()
    if reply != "READY":
        print(f"  FAILED: {remote_path} (header rejected: {reply!r})", file=sys.stderr)
        return False

    # Send in small chunks and wait for each ACK -- a single unbroken write
    # overruns the device's USB-CDC RX buffer. / 一括送信だと受信側バッファが
    # 追いつかないため、小分けにしてACKを待ちながら送る。
    for offset in range(0, len(data), CHUNK_SIZE):
        chunk = data[offset:offset + CHUNK_SIZE]
        ser.write(chunk)
        reply = ser.readline().decode(errors="replace").strip()
        if not reply.startswith("ACK"):
            print(f"  FAILED: {remote_path} (chunk at {offset} rejected: {reply!r})", file=sys.stderr)
            return False

    reply = ser.readline().decode(errors="replace").strip()
    if not reply.startswith("OK"):
        print(f"  FAILED: {remote_path} (final ack missing: {reply!r})", file=sys.stderr)
        return False
    return True


def collect_assets(assets_dir: Path) -> tuple[list[Path], list[Path]]:
    faces = sorted((assets_dir / "face").glob("*.jpg")) if (assets_dir / "face").is_dir() else []
    wavs = sorted((assets_dir / "wav").glob("*.wav")) if (assets_dir / "wav").is_dir() else []
    return faces, wavs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="シリアルポート / serial port (e.g. /dev/ttyACM0)")
    ap.add_argument("--assets", type=Path, default=None,
                    help="送信するアセットのディレクトリ(face/, wav/を含む)。省略時はsd.zipを使用")
    ap.add_argument("--dry-run", action="store_true", help="送信せず一覧表示のみ")
    args = ap.parse_args()

    tmpdir = None
    if args.assets:
        assets_dir = args.assets
    else:
        if not DEFAULT_ZIP.is_file():
            print(f"error: {DEFAULT_ZIP} が見つかりません", file=sys.stderr)
            return 1
        tmpdir = tempfile.TemporaryDirectory(prefix="m5petit_assets_")
        assets_dir = Path(tmpdir.name)
        with zipfile.ZipFile(DEFAULT_ZIP) as z:
            z.extractall(assets_dir)

    faces, wavs = collect_assets(assets_dir)
    if not faces and not wavs:
        print(f"error: {assets_dir} に face/*.jpg も wav/*.wav もありません", file=sys.stderr)
        return 1

    print(f"送信先 / target: {args.port}")
    print(f"顔画像 / faces: {len(faces)}  効果音 / sounds: {len(wavs)}")

    if args.dry_run:
        for f in faces:
            print(f"  (dry-run) face/{f.name}")
        for w in wavs:
            print(f"  (dry-run) wav/{w.name}")
        if tmpdir:
            tmpdir.cleanup()
        return 0

    ok, failed = 0, []
    with serial.Serial(args.port, BAUD, timeout=5) as ser:
        drain_stale(ser)
        for f in faces:
            label = f"face/{f.name}"
            if push_one(ser, f"/{label}", f.read_bytes()):
                print(f"  ok: {label}")
                ok += 1
            else:
                failed.append(label)
        for w in wavs:
            label = f"wav/{w.name}"
            if push_one(ser, f"/{label}", w.read_bytes(), timeout=60):
                print(f"  ok: {label}")
                ok += 1
            else:
                failed.append(label)

    if tmpdir:
        tmpdir.cleanup()

    print(f"\n完了 / done: {ok} uploaded, {len(failed)} failed")
    if failed:
        print("失敗したファイル / failed files:", ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
