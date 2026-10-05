#!/usr/bin/env python3
"""A web rename keeps the highlights this fork stores under the book's content hash.

CrossInk names an EPUB's clipping store after the book's path and moves it when the book
is renamed; scripts/test_web_rename_state.py covers that. This fork names the store after
the book's content, so it needs no move, and the renamed file does not exist yet when the
rename is prepared: the rename must neither be refused because of the store nor touch it.

Run after: pio run -e simulator -j1
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from test_web_rename_state import available_port, store_fixture, tree_contents


ROOT = Path(__file__).resolve().parents[1]
OLD = "/books/Highlighted.epub"
NEW = "/books/Renamed highlighted.epub"
BOOK = b"Book contents must survive a rename.\n"


def content_hash(data: bytes) -> str:
    # KOReaderDocumentId::calculate: MD5 over 1 KB chunks at 0 and 1024 << 2i, i = 0..10.
    md5 = hashlib.md5()
    for i in range(-1, 11):
        offset = 0 if i < 0 else 1024 << (2 * i)
        if offset < len(data):
            md5.update(data[offset : offset + 1024])
    return md5.hexdigest()


def run(program: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="crossink-web-rename-content-") as temporary:
        root = Path(temporary)
        sd = root / "fs_"
        book = sd / OLD.lstrip("/")
        book.parent.mkdir(parents=True)
        book.write_bytes(BOOK)
        store = sd / ".crosspoint/clippings" / f"epub_{content_hash(BOOK)}.bin"
        store.parent.mkdir(parents=True)
        store.write_bytes(store_fixture(OLD, clipping=True))
        store_before = store.read_bytes()

        port = available_port()
        url = f"http://127.0.0.1:{port}"
        env = os.environ.copy()
        # As in test_web_rename_state.py: boot straight into File Transfer over a hotspot.
        env.update(SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_SD=str(sd),
                   CROSSPOINT_SIM_HTTP_PORT=str(port),
                   CROSSPOINT_SIM_SILENT_REBOOT_MAGIC=str(0xC1EAB007),
                   CROSSPOINT_SIM_SILENT_REBOOT_TARGET="6", CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD="2")
        env.pop("CROSSINK_SIMULATOR_SMOKE_TEST", None)
        env.pop("CROSSPOINT_SIM_INPUT_SCRIPT", None)
        log_path = root / "simulator.log"
        with log_path.open("w") as log:
            process = subprocess.Popen([str(program)], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 45
                while True:
                    try:
                        with urllib.request.urlopen(url + "/", timeout=1) as response:
                            if response.status == 200:
                                break
                    except (OSError, urllib.error.URLError):
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("Simulator web portal did not start")
                        time.sleep(0.1)

                body = urllib.parse.urlencode({"path": OLD, "name": Path(NEW).name}).encode()
                try:
                    response = urllib.request.urlopen(urllib.request.Request(url + "/rename", data=body), timeout=10)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    status, message = response.code, response.read().decode()

                assert status == 200, f"rename of a book with content-keyed highlights was refused: {status} {message}"
                assert not book.exists(), "the old filename is still there"
                assert (sd / NEW.lstrip("/")).read_bytes() == BOOK, "the book's bytes changed"
                assert store.read_bytes() == store_before, "the content-keyed clipping store was rewritten"
                strays = [name for name in tree_contents(sd)
                          if name.endswith((".tmp", ".rename.bak")) or name.startswith(".tmp")]
                assert not strays, f"the rename left work files behind: {strays}"
                print("PASS: web rename keeps content-keyed highlights in place")
            except AssertionError:
                print(log_path.read_text()[-4000:])
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, default=ROOT / ".pio/build/simulator/program")
    args = parser.parse_args()
    run(args.program.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
