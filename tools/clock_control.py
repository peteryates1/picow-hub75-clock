#!/usr/bin/env python3
"""Tiny GUI to control the Pico HUB75 clock over MQTT.

Publishes brightness/power commands and the day/night dimming schedule to the
broker. Dependency-free: a minimal built-in MQTT 3.1.1 publisher (QoS 0) over a
socket, plus Tkinter. No paho/mosquitto-clients required.

The schedule settings (day/night levels and hours) are published *retained*, so
the broker remembers them and the clock picks them up on every (re)connect.

Usage:
    python3 tools/clock_control.py [broker_ip]
"""
import random
import socket
import sys
import threading
import tkinter as tk
from tkinter import ttk

DEFAULT_BROKER = "192.168.0.2"
DEFAULT_PREFIX = "picow-clock"
MQTT_PORT = 1883


# --- minimal MQTT publish (CONNECT + PUBLISH + DISCONNECT, QoS 0) -----------

def _remaining_length(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)


def _mqtt_string(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def mqtt_publish(host, port, topic, payload, retain=False,
                 client_id=None, timeout=4):
    """Open a connection, publish one QoS0 message, disconnect.

    Uses a unique client id per call so rapid successive publishes (e.g. the
    schedule's start+end) don't collide and get dropped by the broker.
    """
    if client_id is None:
        client_id = "clock-control-%04x" % random.randint(0, 0xFFFF)
    connect_var = _mqtt_string("MQTT") + bytes([4, 0x02]) + (30).to_bytes(2, "big")
    connect_payload = _mqtt_string(client_id)
    connect = bytes([0x10]) + _remaining_length(
        len(connect_var) + len(connect_payload)) + connect_var + connect_payload

    body = _mqtt_string(topic) + payload.encode()
    header = 0x30 | (0x01 if retain else 0x00)
    publish = bytes([header]) + _remaining_length(len(body)) + body

    with socket.create_connection((host, port), timeout) as s:
        s.settimeout(timeout)
        s.sendall(connect)
        s.recv(4)                       # CONNACK (ignored)
        s.sendall(publish)
        s.sendall(b"\xe0\x00")          # DISCONNECT


# --- GUI --------------------------------------------------------------------

class ClockControl:
    def __init__(self, root, broker):
        self.root = root
        root.title("Pico Clock Control")
        root.resizable(False, False)
        frm = ttk.Frame(root, padding=12)
        frm.grid()
        pad = {"padx": 6, "pady": 3}
        row = 0

        # Broker / prefix
        ttk.Label(frm, text="Broker").grid(row=row, column=0, sticky="w", **pad)
        self.broker = tk.StringVar(value=broker)
        ttk.Entry(frm, textvariable=self.broker, width=15).grid(row=row, column=1, **pad)
        ttk.Label(frm, text="Prefix").grid(row=row, column=2, sticky="e", **pad)
        self.prefix = tk.StringVar(value=DEFAULT_PREFIX)
        ttk.Entry(frm, textvariable=self.prefix, width=12).grid(row=row, column=3, **pad)
        row += 1

        # --- manual (transient) ---
        ttk.Label(frm, text="Manual", font=("TkDefaultFont", 9, "bold")).grid(
            row=row, column=0, sticky="w", pady=(10, 0))
        row += 1
        ttk.Label(frm, text="Brightness").grid(row=row, column=0, sticky="w", **pad)
        self.bright = tk.IntVar(value=160)
        sc = tk.Scale(frm, from_=0, to=255, orient="horizontal", variable=self.bright,
                      length=220)
        sc.grid(row=row, column=1, columnspan=2, **pad)
        sc.bind("<ButtonRelease-1>",
                lambda e: self.pub("brightness/set", str(self.bright.get())))
        ttk.Button(frm, text="Auto",
                   command=lambda: self.pub("brightness/set", "auto")).grid(row=row, column=3, **pad)
        row += 1
        ttk.Label(frm, text="Power").grid(row=row, column=0, sticky="w", **pad)
        ttk.Button(frm, text="On", command=lambda: self.pub("power/set", "ON")).grid(row=row, column=1, **pad)
        ttk.Button(frm, text="Off", command=lambda: self.pub("power/set", "OFF")).grid(row=row, column=2, **pad)
        row += 1

        ttk.Separator(frm, orient="horizontal").grid(
            row=row, column=0, columnspan=4, sticky="ew", pady=8)
        row += 1

        # --- schedule (retained) ---
        ttk.Label(frm, text="Day/night schedule (saved)",
                  font=("TkDefaultFont", 9, "bold")).grid(row=row, column=0, columnspan=3, sticky="w")
        row += 1
        ttk.Label(frm, text="Day level").grid(row=row, column=0, sticky="w", **pad)
        self.day = tk.IntVar(value=160)
        ds = tk.Scale(frm, from_=0, to=255, orient="horizontal", variable=self.day, length=220)
        ds.grid(row=row, column=1, columnspan=2, **pad)
        ds.bind("<ButtonRelease-1>",
                lambda e: self.pub("day/set", str(self.day.get()), retain=True))
        row += 1
        ttk.Label(frm, text="Night level").grid(row=row, column=0, sticky="w", **pad)
        self.night = tk.IntVar(value=40)
        ns = tk.Scale(frm, from_=0, to=255, orient="horizontal", variable=self.night, length=220)
        ns.grid(row=row, column=1, columnspan=2, **pad)
        ns.bind("<ButtonRelease-1>",
                lambda e: self.pub("night/set", str(self.night.get()), retain=True))
        row += 1
        ttk.Label(frm, text="Bright from").grid(row=row, column=0, sticky="w", **pad)
        self.start = tk.IntVar(value=8)
        ttk.Spinbox(frm, from_=0, to=23, width=4, textvariable=self.start).grid(row=row, column=1, sticky="w", **pad)
        ttk.Label(frm, text="to (hour)").grid(row=row, column=2, sticky="e", **pad)
        self.end = tk.IntVar(value=21)
        ttk.Spinbox(frm, from_=0, to=23, width=4, textvariable=self.end).grid(row=row, column=3, sticky="w", **pad)
        row += 1
        ttk.Button(frm, text="Apply schedule hours", command=self.apply_hours).grid(
            row=row, column=1, columnspan=2, **pad)
        row += 1

        self.status = tk.StringVar(value="Ready")
        ttk.Label(frm, textvariable=self.status, foreground="#555").grid(
            row=row, column=0, columnspan=4, sticky="w", pady=(10, 0))

    def pub(self, suffix, payload, retain=False):
        topic = f"{self.prefix.get()}/{suffix}"
        host = self.broker.get().strip()

        def work():
            try:
                mqtt_publish(host, MQTT_PORT, topic, payload, retain=retain)
                msg = f"Sent {topic} = {payload}" + (" (saved)" if retain else "")
            except Exception as e:  # noqa: BLE001
                msg = f"Error: {e}"
            self.root.after(0, lambda: self.status.set(msg))

        self.status.set(f"Sending {topic} = {payload} ...")
        threading.Thread(target=work, daemon=True).start()

    def apply_hours(self):
        self.pub("day_start/set", str(self.start.get()), retain=True)
        self.pub("day_end/set", str(self.end.get()), retain=True)


def main():
    broker = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BROKER
    root = tk.Tk()
    ClockControl(root, broker)
    root.mainloop()


if __name__ == "__main__":
    main()
