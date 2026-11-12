# Generates the CyberDirStat interface sounds (pure synthesis, no samples).
# Usage: python3 make_sounds.py  -> writes scan.wav, delete.wav, purge.wav next to this script
import math, random, struct, wave, pathlib

RATE = 22050
random.seed(2077)

def square(phase): return 1.0 if (phase % 1.0) < 0.5 else -1.0
def env(t, attack, length): return min(1.0, t / attack) * max(0.0, 1.0 - t / length) ** 2
def crush(x, bits=5): q = 2 ** bits; return round(x * q) / q

def scan_done():
    # fast rising arpeggio (A minor 9th) with a detuned square + sine echo: "access granted"
    notes = [440.0, 523.25, 659.25, 987.77]
    out = []
    step = 0.06
    total = step * len(notes) + 0.28
    for i in range(int(total * RATE)):
        t = i / RATE
        s = 0.0
        for n, f in enumerate(notes):
            t0 = n * step
            if t < t0: continue
            lt = t - t0
            length = 0.30 if n == len(notes) - 1 else 0.11
            if lt > length: continue
            e = env(lt, 0.004, length)
            s += e * (0.55 * square(f * lt) + 0.35 * square(f * 1.006 * lt) + 0.5 * math.sin(2 * math.pi * f * 2 * lt))
        out.append(crush(s * 0.33, 6))
    return out

def delete():
    # downward laser zap with a short noise click: "file flatlined"
    out = []
    length = 0.22
    phase = 0.0
    for i in range(int(length * RATE)):
        t = i / RATE
        f = 1400 * (0.12 ** (t / length)) + 90
        phase += f / RATE
        e = env(t, 0.002, length)
        noise = random.uniform(-1, 1) * max(0.0, 1 - t / 0.03)
        out.append(crush(0.45 * e * square(phase) + 0.25 * noise, 5))
    return out

def purge():
    # filtered noise sweep + falling glitch tones + low thump: "data purge complete"
    out = []
    length = 0.62
    lp = 0.0
    phase = 0.0
    for i in range(int(length * RATE)):
        t = i / RATE
        cutoff = 0.02 + 0.5 * max(0.0, 1 - t / 0.35)
        lp += cutoff * (random.uniform(-1, 1) - lp)
        noise = lp * env(t, 0.01, 0.42) * 1.4
        f = 900 * (0.2 ** (t / length))
        if int(t / 0.045) % 3 == 2: f *= 1.5          # stuttering glitch steps
        phase += f / RATE
        tone = 0.35 * square(phase) * env(t, 0.003, length)
        thump = 0.6 * math.sin(2 * math.pi * 55 * t) * max(0.0, 1 - abs(t - 0.36) / 0.12) if t > 0.24 else 0.0
        out.append(crush(0.5 * (noise + tone + thump), 6))
    return out

def write(name, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = 0.55 / peak                                  # moderate loudness, never clips
    fade = int(0.008 * RATE)
    data = bytearray()
    for i, s in enumerate(samples):
        if i >= len(samples) - fade: s *= (len(samples) - i) / fade
        data += struct.pack('<h', int(max(-1.0, min(1.0, s * gain)) * 32767))
    path = pathlib.Path(__file__).with_name(name)
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(bytes(data))
    print(path.name, len(data), 'bytes')

write('scan.wav', scan_done())
write('delete.wav', delete())
write('purge.wav', purge())
