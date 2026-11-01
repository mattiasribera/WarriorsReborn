#!/usr/bin/env python3
"""Per-second level analysis of a 16-bit PCM WAV (e.g. an xbrun --audio-dump).

usage: wav_stats.py FILE.wav [--from SEC] [--to SEC] [--silence DBFS]

Prints one line per second: RMS and peak in dBFS (both channels), left/right RMS, and a bar;
then the number of non-silent seconds (RMS above --silence, default -60 dBFS).
For a spectrogram: ffmpeg -i FILE.wav -lavfi showspectrumpic=s=1024x512:legend=1 FILE.png
"""
import argparse
import array
import math
import sys
import wave


def dbfs(x):
    return 20 * math.log10(x / 32768.0) if x > 0 else -120.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--from", dest="start", type=float, default=0.0)
    ap.add_argument("--to", dest="end", type=float, default=None)
    ap.add_argument("--silence", type=float, default=-60.0)
    a = ap.parse_args()
    w = wave.open(a.wav, "rb")
    if w.getsampwidth() != 2:
        sys.exit("only 16-bit PCM is supported")
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    first = int(a.start * rate)
    last = n if a.end is None else min(n, int(a.end * rate))
    w.setpos(min(first, n))
    print("%s: %d ch, %d Hz, %.1f s" % (a.wav, ch, rate, n / rate))
    loud = 0
    sec = int(a.start)
    pos = first
    while pos < last:
        frames = min(rate, last - pos)
        s = array.array("h")
        s.frombytes(w.readframes(frames))
        if sys.byteorder == "big":
            s.byteswap()
        if not s:
            break
        sq = [0.0] * ch
        peak = 0
        for i, v in enumerate(s):
            sq[i % ch] += v * v
            if abs(v) > peak:
                peak = abs(v)
        per = [math.sqrt(q / (len(s) / ch)) for q in sq]
        rms = math.sqrt(sum(sq) / len(s))
        level = dbfs(rms)
        loud += level > a.silence
        bar = "#" * max(0, int((level + 60) / 2))
        print("%5d s  rms %6.1f  peak %6.1f  L %6.1f  R %6.1f  %s" % (sec, level, dbfs(peak), dbfs(per[0]), dbfs(per[-1]), bar))
        pos += frames
        sec += 1
    print("non-silent seconds (rms > %.0f dBFS): %d of %d" % (a.silence, loud, sec - int(a.start)))


if __name__ == "__main__":
    main()
