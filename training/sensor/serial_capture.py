"""
serial_capture.py — log to your laptop when there is no microSD card.

  pip install pyserial
  python serial_capture.py --port /dev/cu.usbmodem1101      # macOS
  python serial_capture.py --port COM5                      # Windows
  python serial_capture.py --port /dev/ttyACM0              # Linux
  python serial_capture.py --list                           # show ports

The ESP32 streams revision-2 CSV rows prefixed with '>'. This captures them
without an SD card. The archived three-sensor trainers must not consume this
schema. Camera JPEGs are stored separately on device flash.

Rotates to a new file every hour so a disconnect costs you one hour, not
everything. Ctrl-C to stop; the current file is flushed and closed cleanly.
"""
from __future__ import annotations
import argparse, os, sys, time
from datetime import datetime

# Emitted by the firmware at boot, but hardcoded here too so a capture started
# mid-session still gets a valid header.
HEADER = ("ms,epoch,dev2,unused_mq7,dev135,signal2,unused_r7,signal135,sc2,unused_sc7,sc135,tC,rh,hPa,a_gas,"
          "unused_co,a_smoke,m_gas,unused_mco,m_smoke,unused_codose,dT60,dAH60,dRH60,cnn_flame,"
          "cnn_burner,cnn_smkL,cnn_smkH,cnn_person,cnn_pconf,cnn_motion,"
          "cnn_conf,cnn_luma,vis_credit,hint,c_fv,c_sg,c_sv,c_cg,c_te,c_gg,"
          "w_fv,w_sg,w_sv,w_cg,w_te,w_gg,base,bonus,damp,frs,grs,unused_crs,state,"
          "flags,label")
N_COLS = HEADER.count(",") + 1


def list_ports():
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    if not ports:
        print("no serial ports found")
        return
    for p in ports:
        print(f"  {p.device:<24} {p.description}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="logs")
    ap.add_argument("--rotate-min", type=int, default=60)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        list_ports()
        return
    if not a.port:
        sys.exit("give --port (or --list to see what is connected)")

    import serial
    os.makedirs(a.out, exist_ok=True)

    f, path, opened_at, rows, bad = None, None, 0.0, 0, 0

    def rotate():
        nonlocal f, path, opened_at, rows
        if f:
            f.close()
            print(f"\nclosed {path} ({rows} rows)")
        path = os.path.join(a.out, datetime.now().strftime("serial_%Y%m%d_%H%M%S.csv"))
        f = open(path, "w", buffering=1)
        f.write(HEADER + "\n")
        opened_at, rows = time.time(), 0
        print(f"writing {path}")

    print(f"opening {a.port} at {a.baud}. Ctrl-C to stop.")
    with serial.Serial(a.port, a.baud, timeout=2) as ser:
        rotate()
        last_report = time.time()
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith(">"):
                    continue                       # ordinary console output
                line = line[1:]
                if line.startswith("ms,epoch"):
                    continue                       # firmware's own header line
                if line.count(",") + 1 != N_COLS:
                    bad += 1                       # truncated by a reset or noise
                    continue
                f.write(line + "\n")
                rows += 1

                now = time.time()
                if now - last_report > 10:
                    last_report = now
                    mins = (now - opened_at) / 60
                    print(f"\r  {rows} rows  {mins:5.1f} min  "
                          f"{rows/max(mins*60,1):.1f}/s  {bad} malformed", end="")
                if now - opened_at > a.rotate_min * 60:
                    rotate()
        except KeyboardInterrupt:
            pass
        finally:
            if f:
                f.close()
                print(f"\nclosed {path} ({rows} rows, {bad} malformed)")


if __name__ == "__main__":
    main()
