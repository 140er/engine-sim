#!/usr/bin/env python3
"""Play the baked demo and check that pitch follows RPM and shifts do not click."""

import math
import socket
import struct
import subprocess
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIVE = ROOT / "build" / "engine-sim-live"
PROFILE = ROOT / "profiles" / "subaru_ej25.eslb"

KNOTS = [
    (0.0, 950),
    (1.0, 980),
    (1.15, 1500),
    (3.0, 6200),
    (3.001, 3900),
    (4.8, 6400),
    (4.801, 4200),
    (6.2, 6100),
    (6.201, 6100),
    (7.5, 1600),
    (8.0, 1050),
]


def target_rpm(t):
    if t <= KNOTS[0][0]:
        return KNOTS[0][1]
    hi = 1
    while hi < len(KNOTS) - 1 and KNOTS[hi][0] < t:
        hi += 1
    a, b = KNOTS[hi - 1], KNOTS[hi]
    span = b[0] - a[0]
    u = (t - a[0]) / span if span > 0 else 1
    return a[1] + u * (b[1] - a[1])


def read_wav(path):
    with wave.open(str(path)) as handle:
        rate = handle.getframerate()
        count = handle.getnframes()
        frames = handle.readframes(count)
    samples = struct.unpack("<" + "h" * count, frames)
    audio = [sample / 32768.0 for sample in samples]
    return rate, audio


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def estimate_rpm(audio, rate, guess):
    firing = int(rate * 30 / guess)
    lo = max(32, int(firing * 0.6))
    hi = min(len(audio) // 2, int(firing * 1.6))
    if hi <= lo + 2:
        return guess
    mean = sum(audio) / len(audio)
    centered = [sample - mean for sample in audio]
    best = -1e300
    best_lag = firing
    for lag in range(lo, hi, 2):
        corr = dot(centered[:-lag:2], centered[lag::2])
        if corr > best:
            best = corr
            best_lag = lag
    for lag in range(max(lo, best_lag - 2), min(hi, best_lag + 3)):
        corr = dot(centered[:-lag], centered[lag:])
        if corr > best:
            best = corr
            best_lag = lag
    return rate * 30.0 / best_lag


def smooth_rpm(rate, count):
    tau = 0.020
    k = 1.0 - math.exp(-(1.0 / rate) / tau)
    current = target_rpm(0)
    out = [0.0] * count
    for i in range(count):
        current += (target_rpm(i / rate) - current) * k
        out[i] = current
    return out


def check_demo(path):
    rate, audio = read_wav(path)
    if len(audio) < rate * 7:
        raise SystemExit(f"demo wav is short ({len(audio)} samples)")
    rpm = smooth_rpm(rate, len(audio))
    worst = 0
    for t in (1.8, 2.4, 2.9, 3.4, 4.2, 5.2, 5.8, 7.6):
        i = int(t * rate)
        window = audio[i:i + int(0.12 * rate)]
        guess = float(rpm[i])
        est = estimate_rpm(window, rate, guess)
        err = abs(est - guess) / guess
        worst = max(worst, err)
        if err > 0.12:
            raise SystemExit(f"pitch at {t:.1f}s is {est:.0f} rpm, expected about {guess:.0f}")
    for t in (3.0, 4.8):
        i = int(t * rate)
        region = audio[i - int(0.1 * rate):i + int(0.1 * rate)]
        delta = sorted(abs(region[j] - region[j - 1]) for j in range(1, len(region)))
        p99 = delta[min(len(delta) - 1, int(0.99 * (len(delta) - 1)))]
        ratio = delta[-1] / p99 if p99 > 0 else 0
        if ratio > 3.0:
            raise SystemExit(f"shift at {t:.1f}s looks like a click (step ratio {ratio:.2f})")
    idle = audio[int(0.2 * rate):int(0.5 * rate)]
    pull = audio[int(5.5 * rate):int(5.9 * rate)]
    idle_rms = math.sqrt(sum(s * s for s in idle) / len(idle))
    pull_rms = math.sqrt(sum(s * s for s in pull) / len(pull))
    if pull_rms < idle_rms * 1.5:
        raise SystemExit(f"pull is not louder than idle ({pull_rms:.3f} vs {idle_rms:.3f})")
    print(f"demo ok worst_pitch_error={worst:.3f} idle_rms={idle_rms:.3f} pull_rms={pull_rms:.3f}")


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, text=True)
    if result.returncode != 0:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(cmd)}")


def check_http():
    body = (
        '{"NewData":{"FilteredRpms":100,"Rpms":3200,"Throttle":80,'
        '"Gear":"3","SpeedKmh":120,"EngineLoad":70}}'
    ).encode()
    response = (
        b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + str(len(body)).encode()
        + b"\r\nConnection: close\r\n\r\n"
        + body
    )
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(8)
    port = server.getsockname()[1]
    server.settimeout(2)

    import threading

    def serve():
        try:
            while True:
                try:
                    conn, _ = server.accept()
                except socket.timeout:
                    return
                try:
                    conn.recv(4096)
                    conn.sendall(response)
                finally:
                    conn.close()
        finally:
            server.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    wav = Path(tempfile.gettempdir()) / "es-http.wav"
    run([
        str(LIVE),
        "--profile", str(PROFILE),
        "--source", "simhub",
        "--simhub-url", f"http://127.0.0.1:{port}/api/getgamedata",
        "--seconds", "0.6",
        "--wav", str(wav),
    ])
    server.close()
    rate, audio = read_wav(wav)
    est = estimate_rpm(audio[int(0.2 * rate):int(0.5 * rate)], rate, 3200)
    if abs(est - 3200) / 3200 > 0.12:
        raise SystemExit(f"simhub http pitch {est:.0f} rpm, expected 3200")
    print(f"simhub http ok rpm={est:.0f}")


def check_udp():
    wav = Path(tempfile.gettempdir()) / "es-udp.wav"
    proc = subprocess.Popen(
        [
            str(LIVE),
            "--profile", str(PROFILE),
            "--source", "udp",
            "--udp-port", "21377",
            "--seconds", "1.2",
            "--realtime",
            "--wav", str(wav),
        ],
        cwd=ROOT,
    )
    packet = struct.pack("<4sBBHffff", b"EST1", 1, 1, 4, 4500.0, 0.7, 0.0, 30.0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    import time
    time.sleep(0.15)
    end = time.time() + 0.9
    while time.time() < end:
        sock.sendto(packet, ("127.0.0.1", 21377))
        time.sleep(0.02)
    sock.close()
    code = proc.wait(timeout=5)
    if code != 0:
        raise SystemExit(f"udp player exited {code}")
    rate, audio = read_wav(wav)
    est = estimate_rpm(audio[int(0.4 * rate):int(0.8 * rate)], rate, 4500)
    if abs(est - 4500) / 4500 > 0.12:
        raise SystemExit(f"udp pitch {est:.0f} rpm, expected 4500")
    print(f"udp ok rpm={est:.0f}")


def main():
    if not LIVE.exists():
        raise SystemExit(f"missing {LIVE}")
    if not PROFILE.exists():
        raise SystemExit(f"missing {PROFILE}")
    run([str(LIVE), "--self-check"])
    wav = Path(tempfile.gettempdir()) / "es-demo-check.wav"
    run([str(LIVE), "--profile", str(PROFILE), "--demo", "--wav", str(wav)])
    check_demo(wav)
    check_http()
    check_udp()
    print("check_live ok")


if __name__ == "__main__":
    main()
