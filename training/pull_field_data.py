"""Copy the field samples (photos + your right/wrong verdicts) from the fire cam to this project.

From a microSD card (put the card in your PC; the samples are in its FIELD folder):
    python pull_field_data.py --sd E:\\FIELD

Over Wi-Fi (press B2 on the fire cam's READY screen; it shows the address to use):
    python pull_field_data.py --wifi 192.168.1.23
    python pull_field_data.py --wifi 192.168.4.1 --erase-after   # (on the FireCam hotspot)

From the ESP32's own flash (board connected by USB, same port you flash with):
    python pull_field_data.py --port COM5
    python pull_field_data.py --port COM5 --erase-after     # also clear the board's storage

Each pull goes into its own folder field_data/<date_time>/, so nothing is ever overwritten.
Next: python 6_add_field_data.py
"""
import argparse
import datetime
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FIELD_DATA = ROOT / "field_data"
# Must match partitions.csv in the firmware project
STORAGE_OFFSET = 0x490000
STORAGE_SIZE = 0x370000


def esptool(args: list) -> None:
    """Runs esptool (v5 names commands read-flash/erase-region, v4 read_flash/erase_region)."""
    for variant in (args, [a.replace("-", "_") if a in ("read-flash", "erase-region") else a for a in args]):
        cmd = [sys.executable, "-m", "esptool"] + variant
        print(">", " ".join(cmd))
        if subprocess.run(cmd).returncode == 0:
            return
    sys.exit("\n[!] esptool failed. Is the board connected, and the port right? (Close the serial monitor first.)\n"
             "    If it keeps failing: hold BOOT, tap RST, release BOOT, and try again.")


def from_flash(port: str, dest: Path, erase: bool) -> None:
    tmp = Path(tempfile.mkdtemp())
    image = tmp / "storage.bin"
    esptool(["--chip", "esp32s3", "-p", port, "-b", "921600", "read-flash",
             hex(STORAGE_OFFSET), hex(STORAGE_SIZE), str(image)])
    tools = ROOT / "tools" / "idf_fatfs"
    r = subprocess.run([sys.executable, str(tools / "fatfsparse.py"), str(image), "--wl-layer", "detect"],
                       cwd=tmp, env={**__import__("os").environ, "PYTHONPATH": str(tools)})
    field = next((p for p in tmp.rglob("FIELD") if p.is_dir()), None)
    if r.returncode != 0 or field is None:
        sys.exit("\n[!] No FIELD folder in the board's storage. Have you saved any samples (B2/B3) yet?")
    shutil.copytree(field, dest)
    shutil.rmtree(tmp, ignore_errors=True)
    if erase:
        esptool(["--chip", "esp32s3", "-p", port, "erase-region", hex(STORAGE_OFFSET), hex(STORAGE_SIZE)])
        print("Board storage cleared (the fire cam re-creates it on next boot).")


def from_wifi(address: str, dest: Path, erase: bool) -> None:
    import io
    import tarfile
    import urllib.request

    base = address if address.startswith("http") else f"http://{address}"
    base = base.rstrip("/")
    print(f"Downloading {base}/all.tar ...")
    try:
        data = urllib.request.urlopen(f"{base}/all.tar", timeout=60).read()
    except OSError as e:
        sys.exit(f"\n[!] Could not reach the fire cam at {base}: {e}\n"
                 "    Is it in Wi-Fi sync mode (B2), and is this PC on the same Wi-Fi (or the FireCam hotspot)?")
    tmp = Path(tempfile.mkdtemp())
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():  # only plain files inside FIELD/, nothing else
            if m.isfile() and m.name.startswith("FIELD/") and "/" not in m.name[6:] and ".." not in m.name:
                t.extract(m, tmp)
    field = tmp / "FIELD"
    if not (field / "LOG.CSV").exists():
        sys.exit("\n[!] The fire cam has no samples yet (press B2/B3 after a check to save some).")
    shutil.copytree(field, dest)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"Received {len(data) / 1e6:.1f} MB")
    if erase:
        req = urllib.request.Request(f"{base}/clear", method="POST")
        print(urllib.request.urlopen(req, timeout=30).read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--sd", help="the FIELD folder on the microSD card")
    g.add_argument("--port", help="serial port of the board, e.g. COM5 or /dev/ttyUSB0")
    g.add_argument("--wifi", help="address shown on the fire cam in Wi-Fi sync mode, e.g. 192.168.1.23")
    ap.add_argument("--erase-after", action="store_true", help="(Wi-Fi / USB) clear the board's samples after copying")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    dest = FIELD_DATA / stamp
    n = 2
    while dest.exists():
        dest = FIELD_DATA / f"{stamp}_{n}"
        n += 1
    FIELD_DATA.mkdir(exist_ok=True)
    if args.sd:
        src = Path(args.sd)
        if not (src / "LOG.CSV").exists():
            sys.exit(f"[!] {src} has no LOG.CSV. Point --sd at the FIELD folder on the card.")
        shutil.copytree(src, dest)
    elif args.wifi:
        from_wifi(args.wifi, dest, args.erase_after)
    else:
        from_flash(args.port, dest, args.erase_after)

    n = max(0, len((dest / "LOG.CSV").read_text().strip().splitlines()) - 1) if (dest / "LOG.CSV").exists() else 0
    print(f"\nCopied {n} samples to {dest.relative_to(ROOT)}")
    if args.sd:
        print("You can delete the FIELD folder on the card now, or keep collecting: samples already copied\n"
              "before are recognised and skipped by 6_add_field_data.py.")
    print("Next: python 6_add_field_data.py")


if __name__ == "__main__":
    main()
