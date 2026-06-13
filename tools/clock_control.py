#!/usr/bin/env python3
"""Tiny GUI to control the Pico HUB75 clock over MQTT.

Publishes brightness/power commands to the broker. Dependency-free: a minimal
built-in MQTT 3.1.1 publisher (QoS 0) over a socket, plus Tkinter. No paho/
mosquitto-clients required.

Usage:
    python3 tools/clock_control.py [broker_ip]
"""
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


def mqtt_publish(host, port, topic, payload, client_id="clock-control", timeout=4):
    """Open a connection, publish one retained-false QoS0 message, disconnect."""
    connect_var = _mqtt_string("MQTT") + bytes([4, 0x02]) + (30).to_bytes(2, "big")
    connect_payload = _mqtt_string(client_id)
    connect = bytes([0x10]) + _remaining_length(
        len(connect_var) + len(connect_payload)) + connect_var + connect_payload

    body = _mqtt_string(topic) + payload.encode()
    publish = bytes([0x30]) + _remaining_length(len(body)) + body

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
        pad = {"padx": 8, "pady": 4}

        frm = ttk.Frame(root, padding=12)
        frm.grid()

        ttk.Label(frm, text="Broker").grid(row=0, column=0, sticky="w", **pad)
        self.broker = tk.StringVar(value=broker)
        ttk.Entry(frm, textvariable=self.broker, width=16).grid(row=0, column=1, **pad)
        ttk.Label(frm, text="Topic prefix").grid(row=0, column=2, sticky="w", **pad)
        self.prefix = tk.StringVar(value=DEFAULT_PREFIX)
        ttk.Entry(frm, textvariable=self.prefix, width=14).grid(row=0, column=3, **pad)

        ttk.Separator(frm, orient="horizontal").grid(
            row=1, column=0, columnspan=4, sticky="ew", pady=8)

        # Brightness
        ttk.Label(frm, text="Brightness").grid(row=2, column=0, sticky="w", **pad)
        self.bright = tk.IntVar(value=160)
        self.scale = tk.Scale(frm, from_=0, to=255, orient="horizontal",
                              variable=self.bright, length=240, showvalue=True)
        self.scale.grid(row=2, column=1, columnspan=2, **pad)
        self.scale.bind("<ButtonRelease-1>", lambda e: self.send_brightness())
        ttk.Button(frm, text="Auto", command=self.send_auto).grid(row=2, column=3, **pad)

        # Power
        ttk.Label(frm, text="Power").grid(row=3, column=0, sticky="w", **pad)
        ttk.Button(frm, text="On", command=lambda: self.send_power("ON")).grid(row=3, column=1, **pad)
        ttk.Button(frm, text="Off", command=lambda: self.send_power("OFF")).grid(row=3, column=2, **pad)

        self.status = tk.StringVar(value="Ready")
        ttk.Label(frm, textvariable=self.status, foreground="#555").grid(
            row=4, column=0, columnspan=4, sticky="w", pady=(10, 0))

    def _publish(self, topic_suffix, payload):
        topic = f"{self.prefix.get()}/{topic_suffix}"
        host = self.broker.get().strip()

        def work():
            try:
                mqtt_publish(host, MQTT_PORT, topic, payload)
                msg = f"Sent {topic} = {payload}"
            except Exception as e:  # noqa: BLE001
                msg = f"Error: {e}"
            self.root.after(0, lambda: self.status.set(msg))

        self.status.set(f"Sending {topic} = {payload} ...")
        threading.Thread(target=work, daemon=True).start()

    def send_brightness(self):
        self._publish("brightness/set", str(self.bright.get()))

    def send_auto(self):
        self._publish("brightness/set", "auto")

    def send_power(self, state):
        self._publish("power/set", state)


def main():
    broker = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BROKER
    root = tk.Tk()
    ClockControl(root, broker)
    root.mainloop()


if __name__ == "__main__":
    main()
