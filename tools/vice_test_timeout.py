#!/usr/bin/env python3
"""tools/vice_test_timeout.py - regressione del timeout di comm_rx_byte.

Esegue c64/test_timeout.prg in x64sc headless e controlla il colore del bordo:
VERDE = tutti i test passati, ROSSO = timeout fuori tolleranza.

Il test misura quante volte cambia il jiffy clock durante comm_rx_byte() (ring
buffer vuoto: si esce solo per timeout) e verifica i casi 100/500/2000/10000 ms.
Serve a non reintrodurre il bug storico del byte sbagliato: sul C64 il jiffy
clock e' big-endian, il byte che cambia a 60 Hz e' $A2 (non $A0 ne' $A1).

Uso:  python3 tools/vice_test_timeout.py [--cycles N] [--keep]
Richiede x64sc (VICE) e Pillow.
"""
import argparse
import os
import shutil
import subprocess
import sys

BORDER_OK = (98, 213, 50)       # verde (colore 5)
HERE = os.path.dirname(os.path.abspath(__file__))
C64 = os.path.normpath(os.path.join(HERE, "..", "c64"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", default="30000000",
                    help="limite di cicli emulati (default 30M ~ 30 s)")
    ap.add_argument("--keep", action="store_true",
                    help="non cancellare i file temporanei")
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("serve Pillow (pip install pillow)", file=sys.stderr)
        return 2

    for exe in ("cl65", "x64sc"):
        if not shutil.which(exe):
            print(f"{exe} non trovato nel PATH", file=sys.stderr)
            return 2

    tmp = os.path.join("/tmp", "vicetmo")
    os.makedirs(tmp, exist_ok=True)
    prg = os.path.join(tmp, "test_timeout.prg")
    png = os.path.join(tmp, "shot.png")

    print(f"cl65  -> {prg}")
    r = subprocess.run(["cl65", "-t", "c64", "-o", prg,
                        os.path.join(C64, "test_timeout.c"),
                        os.path.join(C64, "comm.s")])
    if r.returncode != 0:
        return 1

    print(f"x64sc -> cicli {args.cycles}")
    subprocess.run(["x64sc", "-default", "-warp", "-sounddev", "dummy",
                    "-limitcycles", args.cycles, "-autostart", prg,
                    "-exitscreenshot", png],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if not os.path.exists(png):
        print("screenshot non prodotto", file=sys.stderr)
        return 1

    im = Image.open(png).convert("RGB")
    border = im.getpixel((3, 3))
    if border == BORDER_OK:
        print(f"OK  (bordo verde {border})")
        rc = 0
    else:
        print(f"FALLITO (bordo {border}, atteso {BORDER_OK})", file=sys.stderr)
        print(f"screenshot da guardare: {png}", file=sys.stderr)
        rc = 1

    if not args.keep and rc == 0:
        for f in (prg, png):
            try:
                os.remove(f)
            except OSError:
                pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
