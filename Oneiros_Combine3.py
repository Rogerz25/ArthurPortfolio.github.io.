"""
TYPE TO TERMINAL TO START: python Oneiros_Combine3.py --port COM7 --name wsg
Oneiros logger: reads the combined ECG + MAX30105 + MPU6050 sketch over serial and saves a
NEW Excel workbook on every run, with separate tabs:

    ECG     - every sample: time, ADC, volts, LeadsOff
    Vitals  - one row per second: time, BPM, SpO2, TempF
    Head    - head pitch/roll/droop, HEAD_RATE_HZ rows per second (default 10)
    Info    - name, start time, sample counts, measured sample rate, droop summary

Expected serial line from the Arduino sketch:
    Time:<ms>,ADC:<0-4095>,Volts:<v>,LeadsOff:<0/1>,BPM:<n>,SpO2:<n>,TempF:<f>,Pitch:<deg>,Roll:<deg>,Droop:<0/1>
(An older sketch without Pitch/Roll/Droop still works; the Head tab is just left out.)

Timestamps come from the Arduino's own millis() clock, so changing the sketch's
SAMPLE_INTERVAL needs no change here.

Head calibration: when the Arduino starts it asks the wearer to look straight ahead and hold
still for 10 s. Its messages are printed here as [Arduino] lines. Data logging begins after.

Safety: every line is also appended live to a plain .csv next to the workbook,
so nothing is lost if the script or PC crashes. The .xlsx is written when you
press Ctrl+C (or the serial port disconnects).

Install:  pip install pyserial openpyxl
"""

import csv
import os
import sys
import time
from datetime import datetime

import serial
import serial.tools.list_ports
from openpyxl import Workbook
from openpyxl.chart import LineChart, Reference
from openpyxl.styles import Font, PatternFill, Alignment

BAUD = 230400
# Save outside OneDrive-synced folders if you can (OneDrive locking corrupted files before)
OUT_DIR = os.path.join(os.path.expanduser("~"), "Oneiros_Logs")

HEAD_RATE_HZ = 10  # rows per second saved to the Head tab (the raw .csv keeps every sample)

# Color per column: (header fill, data font color)
COLORS = {
    "Time (s)":   ("808080", "404040"),
    "Wall Clock": ("A0A0A0", "404040"),
    "ADC":        ("C00000", "C00000"),
    "Volts":      ("7030A0", "7030A0"),
    "LeadsOff":   ("ED7D31", "C55A11"),
    "BPM":        ("2E75B6", "1F4E79"),
    "SpO2":       ("00B0F0", "0070C0"),
    "TempF":      ("70AD47", "375623"),
    "Pitch":      ("BF8F00", "7F6000"),
    "Roll":       ("00A3A3", "006666"),
    "Droop":      ("FF0000", "C00000"),
}


def pick_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        sys.exit("No serial ports found. Plug in the Arduino and close the Arduino IDE Serial Monitor/Plotter.")
    print("Available ports:")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device} - {p.description}")
    choice = input("Pick port number: ").strip()
    return ports[int(choice)].device


def parse_line(line):
    """'Time:1234,ADC:2048,...,Pitch:1.2,Roll:-0.5,Droop:0' -> dict, or None."""
    try:
        d = {}
        for part in line.split(","):
            k, v = part.split(":")
            d[k.strip()] = float(v)
        if "ADC" not in d or "Time" not in d:
            return None
        return d
    except ValueError:
        return None


def style_sheet(ws, headers):
    for col, h in enumerate(headers, start=1):
        fill, _ = COLORS[h]
        c = ws.cell(row=1, column=col, value=h)
        c.font = Font(bold=True, color="FFFFFF")
        c.fill = PatternFill("solid", fgColor=fill)
        c.alignment = Alignment(horizontal="center")
        ws.column_dimensions[c.column_letter].width = 14
    ws.freeze_panes = "A2"


def add_rows(ws, rows, headers):
    fonts = [Font(color=COLORS[h][1]) for h in headers]
    for r in rows:
        ws.append(r)
    for col, font in enumerate(fonts, start=1):
        for cell in ws.iter_cols(min_col=col, max_col=col, min_row=2, max_row=ws.max_row):
            for c in cell:
                c.font = font


def build_workbook(path, name, start, ecg_rows, vitals_rows, head_rows, droop_events, droop_samples):
    wb = Workbook()

    # ---- ECG tab ----
    ws_ecg = wb.active
    ws_ecg.title = "ECG"
    ecg_headers = ["Time (s)", "Wall Clock", "ADC", "Volts", "LeadsOff"]
    style_sheet(ws_ecg, ecg_headers)
    add_rows(ws_ecg, ecg_rows, ecg_headers)
    ws_ecg.column_dimensions["D"].width = 12

    # ---- Vitals tab ----
    ws_v = wb.create_sheet("Vitals")
    v_headers = ["Time (s)", "Wall Clock", "BPM", "SpO2", "TempF"]
    style_sheet(ws_v, v_headers)
    add_rows(ws_v, vitals_rows, v_headers)

    # ---- Head tab ----
    ws_h = None
    if head_rows:
        ws_h = wb.create_sheet("Head")
        h_headers = ["Time (s)", "Wall Clock", "Pitch", "Roll", "Droop"]
        style_sheet(ws_h, h_headers)
        add_rows(ws_h, head_rows, h_headers)

    # ---- Info tab ----
    duration = ecg_rows[-1][0] if ecg_rows else 0
    rate = (len(ecg_rows) - 1) / duration if duration > 0 else 0
    ws_i = wb.create_sheet("Info")
    ws_i.append(["Name", name])
    ws_i.append(["Start", start.strftime("%Y-%m-%d %H:%M:%S")])
    ws_i.append(["ECG samples", len(ecg_rows)])
    ws_i.append(["Duration (s)", round(duration, 1)])
    ws_i.append(["Avg sample rate (Hz)", round(rate, 1)])
    if head_rows:
        pct = 100.0 * droop_samples / len(ecg_rows) if ecg_rows else 0
        ws_i.append(["Droop events", droop_events])
        ws_i.append(["Time in droop (%)", round(pct, 1)])
    ws_i.column_dimensions["A"].width = 22
    ws_i.column_dimensions["B"].width = 24

    # ---- Charts ----
    if len(ecg_rows) > 1:
        # first 10 s so the chart stays readable
        n = sum(1 for r in ecg_rows if r[0] <= 10)
        n = max(n, 2)
        ch = LineChart()
        ch.title = "ECG (first 10 s)"
        ch.y_axis.title = "ADC counts"
        ch.x_axis.title = "Time (s)"
        ch.add_data(Reference(ws_ecg, min_col=3, min_row=1, max_row=n + 1), titles_from_data=True)
        ch.set_categories(Reference(ws_ecg, min_col=1, min_row=2, max_row=n + 1))
        ch.legend = None
        ch.height, ch.width = 9, 22
        ws_ecg.add_chart(ch, "H2")

    if len(vitals_rows) > 1:
        ch = LineChart()
        ch.title = "BPM and SpO2"
        ch.y_axis.title = "Value"
        ch.x_axis.title = "Seconds"
        ch.add_data(Reference(ws_v, min_col=3, max_col=4, min_row=1, max_row=len(vitals_rows) + 1),
                    titles_from_data=True)
        ch.set_categories(Reference(ws_v, min_col=1, min_row=2, max_row=len(vitals_rows) + 1))
        ch.height, ch.width = 9, 22
        ws_v.add_chart(ch, "H2")

        ch2 = LineChart()
        ch2.title = "Temperature (F)"
        ch2.y_axis.title = "Degrees F"
        ch2.x_axis.title = "Seconds"
        ch2.add_data(Reference(ws_v, min_col=5, min_row=1, max_row=len(vitals_rows) + 1),
                     titles_from_data=True)
        ch2.set_categories(Reference(ws_v, min_col=1, min_row=2, max_row=len(vitals_rows) + 1))
        ch2.height, ch2.width = 9, 22
        ws_v.add_chart(ch2, "H21")

    if ws_h is not None and len(head_rows) > 1:
        ch = LineChart()
        ch.title = "Head pitch and roll"
        ch.y_axis.title = "Degrees from forward"
        ch.x_axis.title = "Seconds"
        ch.add_data(Reference(ws_h, min_col=3, max_col=4, min_row=1, max_row=len(head_rows) + 1),
                    titles_from_data=True)
        ch.set_categories(Reference(ws_h, min_col=1, min_row=2, max_row=len(head_rows) + 1))
        ch.height, ch.width = 9, 22
        ws_h.add_chart(ch, "H2")

    wb.save(path)


def main():
    name = input("Name: ").strip() or "Unknown"
    port = pick_port()

    os.makedirs(OUT_DIR, exist_ok=True)
    start = datetime.now()
    stamp = start.strftime("%Y%m%d_%H%M%S")
    safe_name = "".join(ch for ch in name if ch.isalnum() or ch in "-_") or "Unknown"
    base = os.path.join(OUT_DIR, f"Oneiros_{safe_name}_{stamp}")  # new file every run
    xlsx_path, csv_path = base + ".xlsx", base + ".csv"

    ecg_rows, vitals_rows, head_rows = [], [], []
    last = {"BPM": 0, "SpO2": 0, "TempF": 0.0, "Pitch": 0.0, "Roll": 0.0, "Droop": 0}
    first_ms = None
    next_vitals_t = 0.0
    next_head_t = 0.0
    head_interval = 1.0 / HEAD_RATE_HZ
    has_head = False
    prev_droop = 0
    droop_events = 0
    droop_samples = 0

    print(f"\nLogging to {xlsx_path}\nPress Ctrl+C to stop and save.")
    print("The Arduino asks for a 10 s head calibration at startup: look straight ahead and hold still.\n")

    ser = serial.Serial(port, BAUD, timeout=1)
    time.sleep(2)
    # (no input-buffer reset here, so the Arduino's calibration messages are not thrown away)

    with open(csv_path, "w", newline="") as f:
        raw = csv.writer(f)
        raw.writerow(["Time (s)", "Wall Clock", "ADC", "Volts", "LeadsOff", "BPM", "SpO2", "TempF",
                      "Pitch", "Roll", "Droop"])
        try:
            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                d = parse_line(line)
                if d is None:
                    if line:
                        print(f"[Arduino] {line}")  # calibration prompts and errors
                    continue

                # Time from the Arduino's clock, zeroed at the first sample
                if first_ms is None:
                    first_ms = d["Time"]
                t = (d["Time"] - first_ms) / 1000.0

                wall = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                adc = int(d["ADC"])
                volts = round(d.get("Volts", adc * 3.3 / 4095), 4)
                leads = int(d.get("LeadsOff", 0))
                for k in last:
                    if k in d:
                        last[k] = d[k]
                if "Pitch" in d:
                    has_head = True

                ecg_rows.append([round(t, 3), wall, adc, volts, leads])
                raw.writerow([round(t, 3), wall, adc, volts, leads, last["BPM"], last["SpO2"], last["TempF"],
                              last["Pitch"], last["Roll"], int(last["Droop"])])

                if has_head:
                    droop = int(last["Droop"])
                    if droop:
                        droop_samples += 1
                        if not prev_droop:
                            droop_events += 1
                    prev_droop = droop

                    if t >= next_head_t:  # HEAD_RATE_HZ rows per second
                        head_rows.append([round(t, 2), wall, round(last["Pitch"], 1),
                                          round(last["Roll"], 1), droop])
                        next_head_t = t + head_interval

                if t >= next_vitals_t:  # one vitals row per second
                    vitals_rows.append([round(t, 1), wall, int(last["BPM"]), int(last["SpO2"]), last["TempF"]])
                    next_vitals_t = int(t) + 1
                    f.flush()
                    head_txt = (f"  Pitch={last['Pitch']:6.1f}  Roll={last['Roll']:6.1f}"
                                f"{'  DROOP' if int(last['Droop']) else ''}") if has_head else ""
                    print(f"{t:7.1f}s  ADC={adc:4d}  V={volts:.4f}  BPM={int(last['BPM']):3d}  "
                          f"SpO2={int(last['SpO2']):3d}  Temp={last['TempF']:.1f}F"
                          f"{head_txt}{'  LEADS OFF' if leads else ''}")
        except KeyboardInterrupt:
            print("\nStopping...")
        except serial.SerialException as e:
            print(f"\nSerial error: {e}")
        finally:
            ser.close()

    print(f"Building Excel file ({len(ecg_rows)} ECG samples, {len(head_rows)} head rows)...")
    build_workbook(xlsx_path, name, start, ecg_rows, vitals_rows, head_rows, droop_events, droop_samples)
    print(f"Saved: {xlsx_path}\nBackup CSV: {csv_path}")


if __name__ == "__main__":
    main()