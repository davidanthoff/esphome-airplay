"""Click-test analysis: offset and drift of the ESP against a reference speaker.

One microphone records a metronome played on the ESP and a reference AirPlay
speaker (HomePod, Apple TV) grouped together. At the end of the recording the
ESP's amp is muted for ~10 s so only the reference plays. See README.md.

Usage:
    python analyze.py RECORDING OUT_PREFIX [--dist-esp FT --dist-ref FT]

RECORDING can be .wav or anything ffmpeg reads (e.g. an iPhone .m4a), via the
ffmpeg binary bundled with imageio-ffmpeg. Writes OUT_PREFIX.png.

Method, per beat k:
    B_k(t) = a_k * A(t - ta_k) + e_k * E(t - te_k) + noise
A: reference-only click template, averaged over the muted-ESP beats.
E: ESP-only click template, averaged over residuals B_k - a_k*A(t - ta_k).
The offset te_k - ta_k is found by band-passed cross-correlation with
sub-sample (parabolic) interpolation. Negative = the ESP is early.
"""
import argparse
import os
import subprocess
import tempfile

import matplotlib
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, correlate, find_peaks, sosfiltfilt

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SPEED_OF_SOUND = 343.0  # m/s
FT = 0.3048  # m


def load_mono(path):
    if not path.lower().endswith(".wav"):
        import imageio_ffmpeg

        tmp = os.path.join(tempfile.mkdtemp(), "rec.wav")
        subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-nostdin", "-loglevel", "error",
                        "-y", "-i", path, "-ac", "1", "-c:a", "pcm_s16le", tmp], check=True)
        path = tmp
    sr, x = wavfile.read(path)
    if x.ndim > 1:
        x = x.mean(axis=1)
    return sr, x.astype(np.float32) / 32768.0


def segment_beats(sr, x):
    """One click cluster per ~1 s; returns onset times, energy, windows."""
    hp = sosfiltfilt(butter(4, 1500, "highpass", fs=sr, output="sos"), x)
    k = int(0.002 * sr)
    env = np.convolve(np.abs(hp), np.ones(k) / k, mode="same")
    peaks, _ = find_peaks(env, distance=int(0.8 * sr), height=np.median(env) * 20)
    pre, post = int(0.05 * sr), int(0.25 * sr)
    times, energy, waves = [], [], []
    for p in peaks:
        a = max(0, p - int(0.06 * sr))
        onset = a + int(np.argmax(env[a:p + 1] > 0.2 * env[p]))
        lo, hi = onset - pre, onset + post
        if lo < 0 or hi > len(hp):
            continue
        times.append(onset / sr)
        energy.append(float(np.sum(hp[onset:onset + int(0.2 * sr)] ** 2)))
        waves.append(hp[lo:hi])
    waves = sosfiltfilt(butter(4, [1500, 8000], "bandpass", fs=sr, output="sos"),
                        np.stack(waves).astype(np.float64), axis=1)
    return np.array(times), np.array(energy), waves


def reference_only_beats(energy):
    """The final run of clearly quieter beats (ESP muted), minus transitions."""
    level = np.median(energy[: int(0.8 * len(energy))])
    quiet = energy < 0.6 * level
    end = len(energy) - 1
    while end >= 0 and not quiet[end]:
        end -= 1
    if end < 0:
        raise SystemExit("no muted-ESP beats found at the end of the recording")
    start = end
    while start > 0 and quiet[start - 1]:
        start -= 1
    ref = np.zeros(len(energy), bool)
    ref[start + 1:end] = True
    both = np.zeros(len(energy), bool)
    both[: max(0, start - 2)] = True
    return ref, both, level


class Correlator:
    def __init__(self, sr, max_lag_s=0.06):
        self.max_lag = int(max_lag_s * sr)

    def best_lag(self, sig, tmpl):
        c = correlate(sig, tmpl, mode="full", method="fft")
        mid = len(tmpl) - 1
        c = c[mid - self.max_lag: mid + self.max_lag + 1]
        i = int(np.argmax(c))
        frac = 0.0
        if 0 < i < len(c) - 1:
            y0, y1, y2 = c[i - 1], c[i], c[i + 1]
            den = y0 - 2 * y1 + y2
            if den != 0:
                frac = 0.5 * (y0 - y2) / den
        return i - self.max_lag + frac, i - self.max_lag, c[i] / np.dot(tmpl, tmpl)

    def template(self, rows, ref, passes):
        for _ in range(passes):
            acc = np.zeros_like(ref)
            for r in rows:
                acc += shift(r, -self.best_lag(r, ref)[1])
            ref = acc / len(rows)
        return ref


def shift(sig, n):
    out = np.zeros_like(sig)
    if n >= 0:
        out[n:] = sig[: len(sig) - n]
    else:
        out[:n] = sig[-n:]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recording")
    ap.add_argument("out_prefix")
    ap.add_argument("--dist-esp", type=float, help="ESP speaker to mic, in feet")
    ap.add_argument("--dist-ref", type=float, help="reference speaker to mic, in feet")
    args = ap.parse_args()

    sr, x = load_mono(args.recording)
    times, energy, waves = segment_beats(sr, x)
    ref, both, level = reference_only_beats(energy)
    print(f"{len(times)} beats over {times[-1] / 60:.1f} min; "
          f"reference-only beats: {ref.sum()} (energy {np.median(energy[ref]):.3f} vs {level:.3f})")

    cor = Correlator(sr)
    a_tmpl = cor.template(waves[ref], waves[ref][0], 2)
    ta = np.zeros(len(times))
    resid = np.zeros_like(waves)
    for k in range(len(times)):
        lagf, lag, g = cor.best_lag(waves[k], a_tmpl)
        ta[k] = lagf
        resid[k] = waves[k] - g * shift(a_tmpl, lag)
    first = np.where(both & (times < times[0] + 300))[0]
    e_tmpl = cor.template(resid[first], resid[first[0]], 3)
    te = np.zeros(len(times))
    eg = np.zeros(len(times))
    for k in range(len(times)):
        te[k], _, eg[k] = cor.best_lag(resid[k], e_tmpl)

    off = (te - ta) / sr * 1000.0
    used = both & (eg > 0.3)
    o, tm = off[used], times[used] / 60.0
    corr = 0.0
    if args.dist_esp is not None and args.dist_ref is not None:
        # The farther speaker's sound arrives later; remove that from the offset.
        corr = (args.dist_ref - args.dist_esp) * FT / SPEED_OF_SOUND * 1000.0
    print(f"beats used: {used.sum()} of {both.sum()}")
    print(f"at the mic: median {np.median(o):.2f} ms "
          f"(IQR {np.percentile(o, 25):.2f} .. {np.percentile(o, 75):.2f})")
    if corr:
        print(f"distance correction {corr:+.2f} ms -> offset {np.median(o) + corr:+.2f} ms "
              f"(negative = ESP early)")
    mins = np.arange(int(tm.min()), int(tm.max()) + 1)
    med = np.array([np.median(o[(tm >= a) & (tm < a + 1)]) for a in mins])
    slope, _ = np.polyfit(mins + 0.5, med, 1)
    print(f"drift: {slope * 60:+.2f} ms/hour")

    fig, ax = plt.subplots(figsize=(10, 4.5))
    ax.plot(tm, o + corr, ".", ms=2, alpha=0.35, label="per beat")
    ax.plot(mins + 0.5, med + corr, "-o", ms=3, label="1-minute median")
    ax.axhline(0, color="k", lw=0.8)
    ax.set_xlabel("minutes into recording")
    ax.set_ylabel("ESP − reference (ms" + (", distance-corrected)" if corr else " at the mic)"))
    ax.set_title(f"ESP vs reference: {np.median(o) + corr:+.1f} ms, drift {slope * 60:+.1f} ms/h")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(f"{args.out_prefix}.png", dpi=120)


if __name__ == "__main__":
    main()
