#!/usr/bin/env python3
"""M5 Petit に顔画像・効果音を自動送信する / Push face images & sounds to an M5 Petit.

SDカードをPCに挿して手でコピーする代わりに、WiFi接続済みのM5へ
リポジトリ同梱の標準アセット(sd.zip)をHTTPで流し込みます。
Instead of writing the SD card by hand, this pushes the bundled default
assets (sd.zip) to an M5 that is already on your WiFi.

Usage:
    python3 tools/push_sd_assets.py 192.168.1.109
    python3 tools/push_sd_assets.py 192.168.1.109 --assets ./my_assets_dir
    python3 tools/push_sd_assets.py 192.168.1.109 --dry-run

前提 / Requirements:
  - M5側にSDカードが挿さっていること（アセットの保存先のため）
  - curl コマンド（ほぼ全OSに標準搭載）
"""

import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ZIP = REPO_ROOT / "sd.zip"


def curl_upload(host: str, endpoint: str, file_path: Path, timeout: int = 30) -> bool:
    cmd = [
        "curl", "-sf", "-m", str(timeout),
        "-F", f"file=@{file_path}",
        f"http://{host}{endpoint}",
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0


def collect_assets(assets_dir: Path) -> tuple[list[Path], list[Path]]:
    faces = sorted((assets_dir / "face").glob("*.jpg")) if (assets_dir / "face").is_dir() else []
    wavs = sorted((assets_dir / "wav").glob("*.wav")) if (assets_dir / "wav").is_dir() else []
    return faces, wavs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", help="M5のIPアドレス / M5 IP address (e.g. 192.168.1.109)")
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

    print(f"送信先 / target: http://{args.host}")
    print(f"顔画像 / faces: {len(faces)}  効果音 / sounds: {len(wavs)}")

    ok, failed = 0, []
    for f in faces:
        label = f"face/{f.name}"
        if args.dry_run:
            print(f"  (dry-run) {label}")
            continue
        if curl_upload(args.host, "/upload_face", f):
            print(f"  ok: {label}")
            ok += 1
        else:
            print(f"  FAILED: {label}")
            failed.append(label)
    for w in wavs:
        label = f"wav/{w.name}"
        if args.dry_run:
            print(f"  (dry-run) {label}")
            continue
        if curl_upload(args.host, "/upload_wav", w, timeout=60):
            print(f"  ok: {label}")
            ok += 1
        else:
            print(f"  FAILED: {label}")
            failed.append(label)

    if tmpdir:
        tmpdir.cleanup()

    if args.dry_run:
        return 0
    print(f"\n完了 / done: {ok} uploaded, {len(failed)} failed")
    if failed:
        print("失敗したファイル / failed files:", ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
