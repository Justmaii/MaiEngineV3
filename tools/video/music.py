# Telifsiz, sifirdan sentezlenen fon muzigi. numpy ile; disaridan ses dosyasi yok.
# Kullanim: python3 music.py <sure_sn> <cikti.wav> <gecis zamanlari virgulle> <vurgu zamanlari virgulle>
import numpy as np, sys, wave

SR = 44100
dur = float(sys.argv[1]); out = sys.argv[2]
wipes = [float(x) for x in sys.argv[3].split(',')] if len(sys.argv) > 3 and sys.argv[3] else []
hits = [float(x) for x in sys.argv[4].split(',')] if len(sys.argv) > 4 and sys.argv[4] else []
N = int(SR * dur)
L = np.zeros(N); R = np.zeros(N)
rng = np.random.default_rng(7)
BPM = 120; beat = 60 / BPM; bar = 4 * beat

def add(sig, t0, gain=1.0, pan=0.0):
    i = int(t0 * SR)
    if i >= N: return
    sig = sig[: N - i]
    L[i:i + len(sig)] += sig * gain * (1 - pan) ** 0.5 * 1.0
    R[i:i + len(sig)] += sig * gain * (1 + pan) ** 0.5 * 1.0

def env(n, a, d, s_level=0.0, r=None):
    t = np.arange(n) / SR
    e = np.where(t < a, t / max(a, 1e-4), s_level + (1 - s_level) * np.exp(-(t - a) / max(d, 1e-4)))
    return e

def kick():
    n = int(0.45 * SR); t = np.arange(n) / SR
    f = 42 + 110 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * np.exp(-t * 7) * 0.95

def hat(open_=False):
    n = int((0.25 if open_ else 0.06) * SR)
    x = rng.standard_normal(n)
    x = np.diff(np.concatenate([[0], x]))          # yuksek geciren
    return x * np.exp(-np.arange(n) / SR * (14 if open_ else 70)) * 0.18

def clap():
    n = int(0.3 * SR); x = rng.standard_normal(n)
    e = np.exp(-np.arange(n) / SR * 22)
    for k in (0.0, 0.012, 0.024):
        i = int(k * SR); e[i:i + 200] += 0.6
    return np.diff(np.concatenate([[0], x])) * e * 0.22

def midi(m): return 440 * 2 ** ((m - 69) / 12)

def saw(freq, n, detune=0.0):
    t = np.arange(n) / SR
    s = 0
    for d in (-detune, 0, detune):
        s = s + 2 * ((t * freq * (1 + d)) % 1) - 1
    return s / 3

def lowpass(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.zeros_like(x); acc = 0.0
    # tek kutuplu, iki kez: yumusak
    for pas in range(2):
        acc = 0.0
        for i in range(len(x)):
            acc = (1 - a) * x[i] + a * acc
            y[i] = acc
        x = y.copy()
    return y

def pad(chord, n):
    t = np.arange(n) / SR
    s = sum(np.sin(2 * np.pi * midi(m) * t) + 0.35 * np.sin(2 * np.pi * midi(m) * 2.003 * t) for m in chord)
    e = np.minimum(1, t / 0.4) * np.minimum(1, (n / SR - t) / 0.4)
    return s * e / len(chord) * 0.22

def pluck(m, n):
    t = np.arange(n) / SR
    s = saw(midi(m), n, 0.004)
    s = lowpass(s, 2600)
    return s * np.exp(-t * 9) * 0.16

def bass(m, n):
    t = np.arange(n) / SR
    s = np.sin(2 * np.pi * midi(m) * t) + 0.3 * np.sin(2 * np.pi * midi(m) * 2 * t)
    e = np.minimum(1, t / 0.005) * np.exp(-t * 3.5)
    return s * e * 0.34

# Akor dizisi: Am - F - C - G (her biri bir olcu)
CH = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]]
BASS = [45, 41, 48, 43]
ARP = [[69, 72, 76, 72], [65, 69, 72, 69], [64, 67, 72, 67], [67, 71, 74, 71]]

intro = 3.9           # davul girisi
nbars = int(dur / bar) + 1
pl_cache = {}
for b in range(nbars):
    t0 = b * bar; c = b % 4
    add(pad(CH[c], int(bar * SR)), t0, 1.0)
    for k in range(4):
        tb = t0 + k * beat
        if tb >= dur: break
        # bas: vuruslarin arasinda (sidechain hissi)
        if tb >= intro - 0.01:
            add(bass(BASS[c], int(beat * 0.9 * SR)), tb + beat / 2, 1.0)
            add(kick(), tb, 1.0)
            if k in (1, 3): add(clap(), tb, 1.0, 0.1)
            add(hat(), tb + beat / 2, 1.0, 0.35)
            add(hat(), tb + beat / 4, 0.5, -0.35)
            add(hat(), tb + 3 * beat / 4, 0.5, -0.35)
        # arpej 16'lik
        for s in range(4):
            ts = tb + s * beat / 4
            if ts >= dur: break
            m = ARP[c][s] + (12 if (b // 4) % 2 else 0)
            key = m
            if key not in pl_cache: pl_cache[key] = pluck(m, int(0.3 * SR))
            add(pl_cache[key], ts, 0.8 if ts >= intro else 0.45, 0.3 if s % 2 else -0.3)

# Gecislerde "vuus" (yukselen filtreli gurultu)
for w in wipes:
    n = int(0.8 * SR); x = rng.standard_normal(n); t = np.arange(n) / SR
    e = np.sin(np.pi * np.clip(t / 0.8, 0, 1)) ** 2
    add(np.diff(np.concatenate([[0], x])) * e * 0.12, w - 0.35, 1.0)
# Vurgular (MAT anı vb.): derin darbe
for h in hits:
    n = int(1.2 * SR); t = np.arange(n) / SR
    f = 38 + 80 * np.exp(-t * 10)
    s = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 3) * 0.9
    s += rng.standard_normal(n) * np.exp(-t * 18) * 0.15
    add(s, h, 1.0)

# Kapanis: son 3 sn sonme, basta kisa acilis
t = np.arange(N) / SR
fade = np.minimum(1, t / 0.6) * np.clip((dur - t) / 3.0, 0, 1)
mix = np.stack([L, R], 1) * fade[:, None]
mix = np.tanh(mix * 1.1) * 0.9                 # yumusak limitleyici
peak = np.abs(mix).max(); mix = mix / peak * 0.89
data = (mix * 32767).astype(np.int16)
with wave.open(out, 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(data.tobytes())
print("yazildi", out, dur, "sn")
