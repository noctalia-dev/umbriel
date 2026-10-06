#!/usr/bin/env python3
"""Regenerate linear16-v1 vectors with a direct DFT (no production FFT).

Values are integers in units of 1/65535. Input is rounded to float32 before
analysis, matching the helper's input contract. This is an offline oracle,
not a build dependency. Envelope uses an initial 2048/48000-second step.
"""
import cmath
import math
import struct

N = 2048
EDGES = [20, 40, 80, 120, 180, 270, 400, 600, 900, 1350, 2000,
         3000, 4500, 6750, 10000, 15000, 20000]


def f32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def quantize(value):
    return math.floor(min(1, max(0, value)) * 65535 + 0.5)


def reference(samples):
    samples = list(map(f32, samples))
    rms = math.sqrt(sum(x*x for x in samples) / N)
    window = [0.5 - 0.5 * math.cos(2 * math.pi * n / N) for n in range(N)]
    windowed = [x*w for x, w in zip(samples, window)]
    window_power = sum(w*w for w in window)
    power = []
    for k in range(N//2 + 1):
        value = sum(x * cmath.exp(-2j * math.pi * k * n / N)
                    for n, x in enumerate(windowed))
        factor = 1 if k in (0, N//2) else 2
        power.append(factor * abs(value)**2 / (N * window_power))
    bands = []
    width = 48000 / N
    for low, high in zip(EDGES, EDGES[1:]):
        total = 0
        for k, p in enumerate(power):
            left, right = max(0, (k-.5)*width), min(24000, (k+.5)*width)
            overlap = max(0, min(high, right)-max(low, left))
            total += p * overlap / (right-left)
        bands.append(quantize(math.sqrt(total)))
    return [quantize(rms), quantize(max(map(abs, samples))),
            quantize(rms * (1-math.exp(-(N/48000)/.010)))] + bands


vectors = {
    'silence': [0.] * N,
    'dc': [1.] * N,
    'sine': [math.sin(2*math.pi*64*n/N) for n in range(N)],
    'impulse': [1. if n == N//2 else 0. for n in range(N)],
    'two_tones': [.5*math.sin(2*math.pi*32*n/N)+.25*math.sin(2*math.pi*256*n/N)
                  for n in range(N)],
    'band_edge': [math.sin(2*math.pi*900*n/48000) for n in range(N)],
}
if __name__ == '__main__':
    for name, samples in vectors.items():
        print(name, reference(samples))
