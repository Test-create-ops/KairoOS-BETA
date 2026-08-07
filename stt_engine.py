#!/usr/bin/env python3
"""Viteza STT — motore speech-to-text custom di KairoOS.

Implementazione completamente nostra, nessun motore esterno:
  - Feature extraction: MFCC (pre-emfasi, framing, finestra di Hamming,
    FFT, filterbank mel triangolare, DCT-II) + delta + normalizzazione CMVN.
  - Voice Activity Detection (VAD) basata su energia a breve termine.
  - Riconoscimento: Dynamic Time Warping (DTW) con vincolo di finestra
    contro template multi-voce.

Speaker-independent grazie a template generati da piu' voci + CMVN.
"""
import os
import sys
import json
import glob
import argparse
import subprocess
import tempfile
import numpy as np

SAMPLE_RATE = 16000
FRAME_MS = 25
HOP_MS = 10
N_MELS = 26
N_MFCC = 20
FMAX = 8000
PREEMPH = 0.97
DELTA_W = 2
RECOG_THRESH = 0.6
FFT_SIZE = 512

TEMPLATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stt_templates")
VOCAB_FILE = os.path.join(TEMPLATE_DIR, "vocab.json")

MIC_DEVICE = ":0"
MAX_REC_SEC = 20


# ─── Audio I/O ────────────────────────────────────────────────────────────

def load_raw_pcm(path, sr=SAMPLE_RATE):
    """Carica PCM grezzo s16le (il formato prodotto dalla registrazione)."""
    raw = np.fromfile(path, dtype=np.int16)
    return raw.astype(np.float32) / 32768.0


def decode_to_pcm(src):
    """Decode qualsiasi file audio a PCM mono 16 kHz via ffmpeg."""
    try:
        out = subprocess.run(
            ["ffmpeg", "-v", "quiet", "-i", src, "-ac", "1", "-ar", str(SAMPLE_RATE),
             "-f", "s16le", "-"],
            capture_output=True).stdout
    except FileNotFoundError:
        raise RuntimeError("ffmpeg non trovato")
    if not out:
        raise RuntimeError(f"impossibile decodificare {src}")
    return np.frombuffer(out, dtype=np.int16).astype(np.float32) / 32768.0


def record_mic(duration=None):
    """Registra dal microfono host, ritorna segnale float [-1,1] a 16 kHz."""
    args = ["ffmpeg", "-v", "error", "-f", "avfoundation", "-i", MIC_DEVICE,
            "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "s16le"]
    if duration:
        args += ["-t", str(duration)]
    args += ["-"]
    proc = subprocess.run(args, capture_output=True)
    if not proc.stdout:
        raise RuntimeError("registrazione fallita")
    return np.frombuffer(proc.stdout, dtype=np.int16).astype(np.float32) / 32768.0


def generate_wav_from_say(text, voice, out_wav):
    """Genera audio di una frase con una voce TTS di sistema (solo per i template)."""
    with tempfile.NamedTemporaryFile(suffix=".aiff", delete=False) as tf:
        aiff = tf.name
    try:
        subprocess.run(["say", "-v", voice, "-o", aiff, text],
                       check=True, capture_output=True)
        subprocess.run(["ffmpeg", "-v", "quiet", "-y", "-i", aiff,
                        "-ac", "1", "-ar", str(SAMPLE_RATE), out_wav],
                       check=True, capture_output=True)
    finally:
        if os.path.exists(aiff):
            os.unlink(aiff)


# ─── Feature extraction: MFCC ─────────────────────────────────────────────

def _mel_points(n_mels, sr):
    lo = hz_to_mel(0.0)
    hi = hz_to_mel(min(FMAX, sr / 2 - 1))
    mels = np.linspace(lo, hi, n_mels + 2)
    return mel_to_hz(mels)


def hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + f / 700.0)


def mel_to_hz(m):
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def mel_filterbank(n_mels, n_fft, sr):
    pts = _mel_points(n_mels, sr)
    bins = np.floor((n_fft + 1) * pts / sr).astype(int)
    fbank = np.zeros((n_mels, n_fft // 2 + 1))
    for i in range(1, n_mels + 1):
        f = bins[i - 1]
        c = bins[i]
        t = bins[i + 1]
        if c == f:
            continue
        if t > c:
            fbank[i - 1, c:t] = np.linspace(1.0, 0.0, t - c, endpoint=False)
        if c > f:
            fbank[i - 1, f:c] = np.linspace(0.0, 1.0, c - f, endpoint=False)
    return fbank


def dct2(matrix):
    """DCT-II ortonormale su ogni riga (asse 1)."""
    n = matrix.shape[1]
    m = np.arange(n)[None, :]
    k = np.arange(n)[:, None]
    basis = np.cos(np.pi / n * (m + 0.5) * k)
    basis[0] *= 1.0 / np.sqrt(2.0)
    return matrix @ basis.T * np.sqrt(2.0 / n)


def _deltas(feat, w=DELTA_W):
    d = np.zeros_like(feat)
    for t in range(len(feat)):
        num, den = 0.0, 0.0
        for k in range(1, w + 1):
            x0 = feat[t - k] if t - k >= 0 else feat[0]
            x1 = feat[t + k] if t + k < len(feat) else feat[-1]
            num += k * (x1 - x0)
            den += 2.0 * k * k
        if den > 0:
            d[t] = num / den
    return d


def mfcc(signal, sr=SAMPLE_RATE):
    """Calcola MFCC + delta + delta-delta e normalizza con CMVN."""
    if len(signal) < sr * 0.2:
        return np.zeros((0, N_MFCC * 3))
    n_fft = int(sr * FRAME_MS / 1000)
    hop = int(sr * HOP_MS / 1000)

    pre = np.empty_like(signal)
    pre[0] = signal[0]
    pre[1:] = signal[1:] - PREEMPH * signal[:-1]

    n_frames = 1 + (len(pre) - n_fft) // hop
    if n_frames < 1:
        return np.zeros((0, N_MFCC * 3))
    frames = np.stack([pre[i * hop:i * hop + n_fft] for i in range(n_frames)])
    frames *= np.hamming(n_fft)

    power = np.abs(np.fft.rfft(frames, n=FFT_SIZE, axis=1)) ** 2
    fbank = mel_filterbank(N_MELS, FFT_SIZE, sr)
    mel = power @ fbank.T
    logmel = np.log(mel + 1e-10)

    dct = dct2(logmel)[:, :N_MFCC]

    d1 = _deltas(dct)
    d2 = _deltas(d1)
    feats = np.hstack([dct, d1, d2])

    mu = feats.mean(axis=0, keepdims=True)
    sd = feats.std(axis=0, keepdims=True) + 1e-8
    feats = (feats - mu) / sd
    norms = np.linalg.norm(feats, axis=1, keepdims=True) + 1e-8
    return feats / norms


# ─── VAD: trova l'intervallo parlato ──────────────────────────────────────

def vad_trim(signal, sr=SAMPLE_RATE):
    """Taglia il silenzio e seleziona il segmento parlato principale."""
    n_fft = int(sr * FRAME_MS / 1000)
    hop = int(sr * HOP_MS / 1000)
    if len(signal) < n_fft:
        return signal
    n_frames = 1 + (len(signal) - n_fft) // hop
    energies = np.array([
        np.sum(signal[i * hop:i * hop + n_fft] ** 2)
        for i in range(n_frames)
    ])
    if energies.size == 0:
        return signal
    emax = energies.max()
    if emax <= 0:
        return signal
    thr = emax * 0.12
    voiced = energies > thr
    idx = np.where(voiced)[0]
    if idx.size == 0:
        idx = np.where(energies >= energies.mean())[0]
    if idx.size == 0:
        return signal
    # Unisci i run separati da piccoli gap (<=25 frame = 250ms)
    runs = []
    start = idx[0]
    prev = idx[0]
    for i in idx[1:]:
        if i - prev > 26:
            runs.append((start, prev))
            start = i
        prev = i
    runs.append((start, prev))
    # Scegli il run con l'energia totale maggiore (il parlato principale)
    best = max(runs, key=lambda r: int(energies[r[0]:r[1] + 1].sum()))
    pad = 3
    start = max(0, best[0] - pad)
    end = min(n_frames - 1, best[1] + pad)
    return signal[start * hop:min(len(signal), end * hop + n_fft)]


# ─── DTW ──────────────────────────────────────────────────────────────────

def _len_norm(feat, n=32):
    """Risample la sequenza di feature a esattamente n frame."""
    feat = np.asarray(feat, dtype=np.float64)
    if len(feat) == n:
        return feat
    idx = np.linspace(0, len(feat) - 1, n)
    return np.stack([feat[int(round(i))] for i in idx])


def dtw_dist(a, b, window_ratio=0.2):
    """Distanza DTW normalizzata tra due sequenze MFCC, con finestra di Sakoe-Chiba."""
    a = _len_norm(a)
    b = _len_norm(b)
    n, m = len(a), len(b)
    if n == 0 or m == 0:
        return 1e9
    band = max(int(max(n, m) * window_ratio), 1)
    d = np.full((n + 1, m + 1), np.inf)
    d[0, 0] = 0.0
    for i in range(1, n + 1):
        lo = max(1, i - band)
        hi = min(m, i + band)
        for j in range(lo, hi + 1):
            c = float(np.sum((a[i - 1] - b[j - 1]) ** 2))
            d[i, j] = c + min(d[i - 1, j], d[i, j - 1], d[i - 1, j - 1])
    return d[n, m] / (n + m)


# ─── Template store ───────────────────────────────────────────────────────

def _load_vocab():
    if os.path.exists(VOCAB_FILE):
        with open(VOCAB_FILE) as f:
            return json.load(f)
    return {}


def _save_vocab(vocab):
    os.makedirs(TEMPLATE_DIR, exist_ok=True)
    with open(VOCAB_FILE, "w") as f:
        json.dump(vocab, f, ensure_ascii=False, indent=2)


def load_templates():
    """Ritorna {frase: [matrice MFCC, ...]}."""
    vocab = _load_vocab()
    out = {}
    for phrase, files in vocab.items():
        feats = []
        for rel in files:
            p = os.path.join(TEMPLATE_DIR, rel)
            if os.path.exists(p):
                feats.append(np.load(p))
        if feats:
            out[phrase] = feats
    return out


def add_template(phrase, signal, sr=SAMPLE_RATE):
    """Aggiunge un template MFCC per una frase."""
    sig = vad_trim(signal, sr)
    if len(sig) < sr * 0.25:
        raise RuntimeError("audio troppo corto per il training")
    rms = float(np.sqrt(np.mean(sig ** 2))) if len(sig) else 0.0
    if rms < 0.01:
        raise RuntimeError("nessun parlato rilevato (audio troppo basso)")
    feats = mfcc(sig, sr)
    if len(feats) < 3:
        raise RuntimeError("audio senza parlato rilevato")
    os.makedirs(TEMPLATE_DIR, exist_ok=True)
    vocab = _load_vocab()
    files = vocab.get(phrase, [])
    n = len(files) + 1
    name = f"tpl_{len(vocab)}_{n:02d}.npy"
    path = os.path.join(TEMPLATE_DIR, name)
    np.save(path, feats)
    files.append(name)
    vocab[phrase] = files
    _save_vocab(vocab)
    return name


def train_from_wav(phrase, wav_path):
    add_template(phrase, decode_to_pcm(wav_path))


def generate_templates(phrase, voices):
    """Genera template multi-voce per una frase usando la TTS di sistema."""
    made = []
    for v in voices:
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
            wav = tf.name
        try:
            generate_wav_from_say(phrase, v, wav)
            add_template(phrase, decode_to_pcm(wav))
            made.append(v)
        finally:
            if os.path.exists(wav):
                os.unlink(wav)
    return made


# ─── Riconoscimento ───────────────────────────────────────────────────────

def recognize_signal(signal, sr=SAMPLE_RATE):
    """Riconosce la frase parlata. Ritorna (frase, confidenza)."""
    sig = np.asarray(signal, dtype=np.float64)
    sig = vad_trim(sig, sr)
    if len(sig) < sr * 0.2:
        return None, 0.0
    peak = float(np.abs(sig).max()) if len(sig) else 0.0
    if peak > 1e-6:
        sig = sig * (0.85 / peak)
    feats = mfcc(sig, sr)
    if len(feats) < 3:
        return None, 0.0
    templates = load_templates()
    if not templates:
        return None, 0.0

    best = (None, 1e9)
    second = 1e9
    for phrase, tlist in templates.items():
        for t in tlist:
            dd = dtw_dist(feats, t)
            if dd < best[1]:
                second = best[1]
                best = (phrase, dd)
            elif dd < second:
                second = dd

    if best[0] is None or best[1] >= RECOG_THRESH:
        return None, 0.0
    margin = 1.0 - (best[1] / second) if second < 1e8 else 0.0
    abs_conf = 1.0 - best[1] / RECOG_THRESH
    conf = max(0.0, min(1.0, 0.5 * abs_conf + 0.5 * max(0.0, margin)))
    return best[0], conf


def recognize_file(path):
    return recognize_signal(decode_to_pcm(path))


# ─── CLI ──────────────────────────────────────────────────────────────────

def _cli():
    ap = argparse.ArgumentParser(prog="stt_engine",
                                 description="Motore STT custom KairoOS (MFCC+DTW)")
    sub = ap.add_subparsers(dest="cmd")

    g = sub.add_parser("gen", help="genera template multi-voce con la TTS di sistema")
    g.add_argument("phrase")
    g.add_argument("--voices", default="Alice,Eddy,Flo,Grandma,Reed,Rocko,Sandy,Shelley")

    t = sub.add_parser("train", help="allena un template dal microfono")
    t.add_argument("phrase")
    t.add_argument("--seconds", type=float, default=4.0)

    r = sub.add_parser("rec", help="riconosce una frase dal microfono")
    r.add_argument("--seconds", type=float, default=5.0)

    f = sub.add_parser("file", help="riconosce un file audio")
    f.add_argument("path")

    v = sub.add_parser("vocab", help="mostra il vocabolario")
    a = ap.parse_args()

    if a.cmd == "gen":
        voices = [x.strip() for x in a.voices.split(",") if x.strip()]
        made = generate_templates(a.phrase, voices)
        print(f"template generati per '{a.phrase}' da {len(made)} voci: {', '.join(made)}")
    elif a.cmd == "train":
        sig = record_mic(duration=a.seconds)
        name = add_template(a.phrase, sig)
        print(f"template '{a.phrase}' salvato: {name}")
    elif a.cmd == "rec":
        sig = record_mic(duration=a.seconds)
        phrase, conf = recognize_signal(sig)
        if phrase:
            print(f"RICONOSCIUTO: {phrase}  (conf {conf:.2f})")
        else:
            print("NON RICONOSCIUTO")
    elif a.cmd == "file":
        phrase, conf = recognize_file(a.path)
        if phrase:
            print(f"RICONOSCIUTO: {phrase}  (conf {conf:.2f})")
        else:
            print("NON RICONOSCIUTO")
    elif a.cmd == "vocab":
        vc = _load_vocab()
        if not vc:
            print("(vuoto)")
        for phrase, files in vc.items():
            print(f"{phrase}: {len(files)} template")
    else:
        ap.print_help()


if __name__ == "__main__":
    _cli()
