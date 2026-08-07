// Viteza STT — motore speech-to-text custom portato in C nel kernel.
// MFCC + VAD + DTW, dati precalcolati in stt_data.h (generati da Python).
// Freestanding: nessuna libc, niente <math.h>; log/sqrt implementati qui.
#include <stdint.h>

#include "stt_data.h"

#define SR 16000
#define NWIN STT_NWIN
#define HOP STT_HOP
#define NFFT STT_NFFT
#define N_MELS STT_MELS
#define N_COEF STT_NCOEF
#define N_FEAT STT_NFEAT
#define PREEMPH 0.97
#define DELTA_W 2
#define RECOG_THRESH 0.6
#define MAX_FRAMES 256
#define T_FRAMES STT_TFRAMES

// Buffers statici (single-thread; la stack del kernel e' limitata a ~576KB)
static float stt_buf[MAX_FRAMES * HOP + NWIN];
static float stt_mono[MAX_FRAMES * HOP + NWIN];
static float stt_feat[MAX_FRAMES * N_FEAT];
static float stt_base[MAX_FRAMES][N_COEF];
static double stt_pow[NFFT / 2 + 1];
static double stt_mel[N_MELS];
static double stt_lm[N_MELS];
static double stt_ener[MAX_FRAMES];
static int stt_voiced[MAX_FRAMES];
static double stt_re[NFFT], stt_im[NFFT];
static double stt_dtwd[34][34];

// ─── math helpers (macro: il kernel e' compilato -mno-sse, quindi nessuna
// funzione puo' ritornare float/double) ──────────────────────────────────

#define STT_LOG(x, out) do { \
    double _x = (x); double _o; \
    if (_x <= 0.0) _o = -40.0; \
    else { \
        int _e = 0; \
        while (_x >= 2.0) { _x *= 0.5; _e++; } \
        while (_x < 1.0) { _x *= 2.0; _e--; } \
        double _y = (_x - 1.0) / (_x + 1.0); \
        double _y2 = _y * _y; \
        double _s = _y; \
        double _term = _y; \
        for (int _i = 1; _i <= 8; _i++) { _term *= _y2; _s += _term / (double)(2 * _i + 1); } \
        _o = 2.0 * _s + _e * 0.6931471805599453; \
    } \
    (out) = _o; \
} while (0)

#define STT_SQRT(x, out) do { \
    double _x = (x); double _r = _x; \
    if (_x <= 0.0) _r = 0.0; \
    else { \
        for (int _i = 0; _i < 24; _i++) { \
            double _nr = 0.5 * (_r + _x / _r); \
            if (_nr == _r) break; \
            _r = _nr; \
        } \
    } \
    (out) = _r; \
} while (0)

// ─── FFT radix-2 (512 punti, twiddle da tabelle) ─────────────────────────

static void stt_fft(double *re, double *im) {
    int n = NFFT;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < (len >> 1); j++) {
                int idx = (j * step) & (n - 1);
                double wr = stt_cos_tab[idx];
                double wi = -stt_sin_tab[idx];
                double vr = re[i + j + (len >> 1)];
                double vi = im[i + j + (len >> 1)];
                double tr = vr * wr - vi * wi;
                double ti = vr * wi + vi * wr;
                double ur = re[i + j];
                double ui = im[i + j];
                re[i + j] = ur + tr;
                im[i + j] = ui + ti;
                re[i + j + (len >> 1)] = ur - tr;
                im[i + j + (len >> 1)] = ui - ti;
            }
        }
    }
}

// ─── MFCC ────────────────────────────────────────────────────────────────
// ritorna il numero di frame; out = [frame][N_FEAT] (gia' normalizzato)

int stt_mfcc(const float *sig, int n, float *out) {
    if (n < SR / 5) return 0;
    int n_frames = 1 + (n - NWIN) / HOP;
    if (n_frames < 1) return 0;
    if (n_frames > MAX_FRAMES) n_frames = MAX_FRAMES;

    // pre-emphasis
    float pre[NWIN];

    for (int f = 0; f < n_frames; f++) {
        const float *s = sig + f * HOP;
        pre[0] = s[0];
        for (int i = 1; i < NWIN; i++) pre[i] = s[i] - (float)PREEMPH * s[i - 1];
        for (int i = 0; i < NWIN; i++) pre[i] *= (float)stt_hamming[i];

        for (int i = 0; i < NFFT; i++) { stt_re[i] = (i < NWIN) ? pre[i] : 0.0; stt_im[i] = 0.0; }
        stt_fft(stt_re, stt_im);
        for (int i = 0; i < NFFT / 2 + 1; i++) stt_pow[i] = stt_re[i] * stt_re[i] + stt_im[i] * stt_im[i];

        // mel filterbank
        for (int m = 0; m < N_MELS; m++) {
            double acc = 0.0;
            const float *fb = stt_mel_bank[m];
            for (int i = 0; i < NFFT / 2 + 1; i++) acc += stt_pow[i] * fb[i];
            stt_mel[m] = acc;
        }
        // log + dct
        for (int m = 0; m < N_MELS; m++) STT_LOG(stt_mel[m] + 1e-10, stt_lm[m]);
        float *fr = out + f * N_FEAT;
        for (int k = 0; k < N_COEF; k++) {
            double acc = 0.0;
            for (int m = 0; m < N_MELS; m++) acc += stt_lm[m] * stt_dct_mat[k][m];
            fr[k] = (float)acc;
        }
    }

    // deltas e delta-deltas (sul blocco, dopo CMVN? no: prima CMVN come python)
    // python: feats = hstack([dct, d1, d2]); poi CMVN. Calcoliamo d1,d2 sui coeff.
    // le feature dct sono gia' in out[0..20); salviamo i coeff grezzi per delta
    for (int f = 0; f < n_frames; f++)
        for (int d = 0; d < N_COEF; d++) stt_base[f][d] = out[f * N_FEAT + d];

    for (int t = 0; t < n_frames; t++) {
        for (int d = 0; d < N_COEF; d++) {
            double num = 0.0, den = 0.0;
            for (int k = 1; k <= DELTA_W; k++) {
                int i0 = t - k; if (i0 < 0) i0 = 0;
                int i1 = t + k; if (i1 >= n_frames) i1 = n_frames - 1;
                double x0 = stt_base[i0][d], x1 = stt_base[i1][d];
                num += k * (x1 - x0);
                den += 2.0 * (double)k * (double)k;
            }
            out[t * N_FEAT + N_COEF + d] = (den > 0.0) ? (float)(num / den) : 0.0f;
        }
    }
    for (int t = 0; t < n_frames; t++) {
        for (int d = 0; d < N_COEF; d++) {
            double num = 0.0, den = 0.0;
            for (int k = 1; k <= DELTA_W; k++) {
                int i0 = t - k; if (i0 < 0) i0 = 0;
                int i1 = t + k; if (i1 >= n_frames) i1 = n_frames - 1;
                double x0 = out[i0 * N_FEAT + N_COEF + d];
                double x1 = out[i1 * N_FEAT + N_COEF + d];
                num += k * (x1 - x0);
                den += 2.0 * (double)k * (double)k;
            }
            out[t * N_FEAT + 2 * N_COEF + d] = (den > 0.0) ? (float)(num / den) : 0.0f;
        }
    }

    // CMVN
    double mu[N_FEAT], sd[N_FEAT];
    for (int d = 0; d < N_FEAT; d++) {
        double m = 0.0;
        for (int t = 0; t < n_frames; t++) m += out[t * N_FEAT + d];
        m /= n_frames;
        double v = 0.0;
        for (int t = 0; t < n_frames; t++) { double x = out[t * N_FEAT + d] - m; v += x * x; }
        v /= n_frames;
        mu[d] = m;
        STT_SQRT(v, sd[d]);
        sd[d] += 1e-8;
    }
    for (int t = 0; t < n_frames; t++)
        for (int d = 0; d < N_FEAT; d++)
            out[t * N_FEAT + d] = (float)((out[t * N_FEAT + d] - mu[d]) / sd[d]);

    // L2 row norm
    for (int t = 0; t < n_frames; t++) {
        double acc = 0.0;
        for (int d = 0; d < N_FEAT; d++) { double x = out[t * N_FEAT + d]; acc += x * x; }
        double norm;
        STT_SQRT(acc, norm);
        norm += 1e-8;
        for (int d = 0; d < N_FEAT; d++) out[t * N_FEAT + d] = (float)(out[t * N_FEAT + d] / norm);
    }
    return n_frames;
}

// ─── VAD ─────────────────────────────────────────────────────────────────
// taglia silenzio e seleziona il run di parlato principale; ritorna nuova lunghezza

int stt_vad_trim(float *sig, int n) {
    if (n < NWIN) return n;
    int n_frames = 1 + (n - NWIN) / HOP;
    if (n_frames > MAX_FRAMES) n_frames = MAX_FRAMES;
    double emax = 0.0;
    for (int i = 0; i < n_frames; i++) {
        double acc = 0.0;
        const float *s = sig + i * HOP;
        for (int j = 0; j < NWIN; j++) acc += (double)s[j] * s[j];
        stt_ener[i] = acc;
        if (acc > emax) emax = acc;
    }
    if (emax <= 0.0) return n;
    double thr = emax * 0.12;
    int nv = 0;
    double emean = 0.0;
    for (int i = 0; i < n_frames; i++) emean += stt_ener[i];
    emean /= n_frames;
    for (int i = 0; i < n_frames; i++)
        if (stt_ener[i] > thr) stt_voiced[nv++] = i;
    if (nv == 0)
        for (int i = 0; i < n_frames; i++)
            if (stt_ener[i] >= emean) stt_voiced[nv++] = i;
    if (nv == 0) return n;

    // run principali (merge gap <=25 frame)
    int best_s = stt_voiced[0], best_e = stt_voiced[0], best_e2 = 0;
    int run_s = stt_voiced[0], run_p = stt_voiced[0];
    int cur_s = stt_voiced[0], cur_e = stt_voiced[0];
    double cur_sum = stt_ener[stt_voiced[0]];
    int i = 1;
    while (i < nv) {
        if (stt_voiced[i] - stt_voiced[i - 1] > 26) {
            // chiudi run
            if (cur_sum > best_e2) { best_s = cur_s; best_e = cur_e; best_e2 = (int)cur_sum; }
            cur_s = stt_voiced[i]; cur_e = stt_voiced[i];
            cur_sum = stt_ener[stt_voiced[i]];
        } else {
            cur_e = stt_voiced[i];
            cur_sum += stt_ener[stt_voiced[i]];
        }
        i++;
    }
    if (cur_sum > best_e2) { best_s = cur_s; best_e = cur_e; best_e2 = (int)cur_sum; }

    int pad = 3;
    int start = best_s - pad; if (start < 0) start = 0;
    int end = best_e + pad;   if (end > n_frames - 1) end = n_frames - 1;
    int newlen = end * HOP + NWIN;
    if (newlen > n) newlen = n;
    int off = start * HOP;
    if (off > 0) {
        for (int j = 0; j < newlen - off; j++) sig[j] = sig[off + j];
        newlen -= off;
    }
    (void)run_s; (void)run_p; (void)best_e2;
    return newlen;
}

// ─── risample a T_FRAMES ─────────────────────────────────────────────────

static void stt_len_norm32(const float *feat, int nframes, float *out) {
    for (int i = 0; i < T_FRAMES; i++) {
        double fi = (double)i * (nframes - 1) / (double)(T_FRAMES - 1);
        int idx = (int)(fi + 0.5);
        if (idx < 0) idx = 0;
        if (idx >= nframes) idx = nframes - 1;
        for (int d = 0; d < N_FEAT; d++) out[i * N_FEAT + d] = feat[idx * N_FEAT + d];
    }
}

// ─── DTW (entrambe 32 frame, banda Sakoe-Chiba) ──────────────────────────

#define INF 1e18

static void stt_dtw(const float *q, const float *t, double *out) {
    for (int i = 0; i <= T_FRAMES; i++)
        for (int j = 0; j <= T_FRAMES; j++) stt_dtwd[i][j] = INF;
    stt_dtwd[0][0] = 0.0;
    int band = 6;
    for (int i = 1; i <= T_FRAMES; i++) {
        int lo = i - band; if (lo < 1) lo = 1;
        int hi = i + band; if (hi > T_FRAMES) hi = T_FRAMES;
        for (int j = lo; j <= hi; j++) {
            double c = 0.0;
            const float *qa = q + (i - 1) * N_FEAT;
            const float *tb = t + (j - 1) * N_FEAT;
            for (int d = 0; d < N_FEAT; d++) { double x = qa[d] - tb[d]; c += x * x; }
            double m = stt_dtwd[i - 1][j];
            if (stt_dtwd[i][j - 1] < m) m = stt_dtwd[i][j - 1];
            if (stt_dtwd[i - 1][j - 1] < m) m = stt_dtwd[i - 1][j - 1];
            stt_dtwd[i][j] = c + m;
        }
    }
    *out = stt_dtwd[T_FRAMES][T_FRAMES] / (double)(2 * T_FRAMES);
}

// ─── riconoscimento su segnale mono 16k ──────────────────────────────────
// ritorna l'indice della frase (>=0) oppure -1
// gli indici >= STT_NPHRASE si riferiscono a template di sessione

static float q32[T_FRAMES * N_FEAT];
static float tpl32[T_FRAMES * N_FEAT];

int stt_recognize_mono(const float *sig, int n);

#define STT_MAX_SESS 8
static int16_t stt_sess_tpl[STT_MAX_SESS][T_FRAMES * N_FEAT];
static char stt_sess_name[STT_MAX_SESS][32];
static int stt_sess_count = 0;

int stt_session_count(void) { return stt_sess_count; }

double stt_last_dist = 0.0;   // distanza DTW del best match (diagnostica)
int stt_last_best = -1;

int stt_train_pcm16(const int16_t *s16, int n, const char *name) {
    if (stt_sess_count >= STT_MAX_SESS) return -1;
    int idx = stt_sess_count;
    int cap = MAX_FRAMES * HOP + NWIN;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) stt_mono[i] = s16[i] / 32768.0f;
    int n2 = stt_vad_trim(stt_mono, n);
    if (n2 < SR / 5) return -1;
    double peak = 0.0;
    for (int i = 0; i < n2; i++) { double a = stt_mono[i] < 0 ? -stt_mono[i] : stt_mono[i]; if (a > peak) peak = a; }
    if (peak > 1e-6) {
        float g = (float)(0.85 / peak);
        for (int i = 0; i < n2; i++) stt_mono[i] *= g;
    }
    int nf = stt_mfcc(stt_mono, n2, stt_feat);
    if (nf < 3) return -1;
    stt_len_norm32(stt_feat, nf, q32);
    for (int i = 0; i < T_FRAMES * N_FEAT; i++)
        stt_sess_tpl[idx][i] = (int16_t)(q32[i] * STT_TSCALE);
    int j;
    for (j = 0; name[j] && j < 31; j++) stt_sess_name[idx][j] = name[j];
    stt_sess_name[idx][j] = 0;
    stt_sess_count++;
    return idx;
}

int stt_recognize_pcm16(const int16_t *s16, int n) {
    int cap = MAX_FRAMES * HOP + NWIN;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) stt_mono[i] = s16[i] / 32768.0f;
    return stt_recognize_mono(stt_mono, n);
}

int stt_recognize_mono(const float *sig, int n) {
    if (n > MAX_FRAMES * HOP + NWIN) n = MAX_FRAMES * HOP + NWIN;
    for (int i = 0; i < n; i++) stt_buf[i] = sig[i];

    int n2 = stt_vad_trim(stt_buf, n);
    if (n2 < SR / 5) return -1;

    double peak = 0.0;
    for (int i = 0; i < n2; i++) { double a = stt_buf[i] < 0 ? -stt_buf[i] : stt_buf[i]; if (a > peak) peak = a; }
    if (peak > 1e-6) {
        float g = (float)(0.85 / peak);
        for (int i = 0; i < n2; i++) stt_buf[i] *= g;
    }

    int nf = stt_mfcc(stt_buf, n2, stt_feat);
    if (nf < 3) return -1;

    stt_len_norm32(stt_feat, nf, q32);

    int best = -1;
    double bd = 1e9, second = 1e9;
    for (int p = 0; p < STT_NPHRASE; p++) {
        for (int t = 0; t < STT_NTPL; t++) {
            for (int i = 0; i < T_FRAMES * N_FEAT; i++)
                tpl32[i] = (float)(stt_templates[p][t][i] / STT_TSCALE);
            double dd;
            stt_dtw(q32, tpl32, &dd);
            if (dd < bd) { second = bd; bd = dd; best = p; }
            else if (dd < second) second = dd;
        }
    }
    for (int s = 0; s < stt_sess_count; s++) {
        for (int i = 0; i < T_FRAMES * N_FEAT; i++)
            tpl32[i] = (float)(stt_sess_tpl[s][i] / STT_TSCALE);
        double dd;
        stt_dtw(q32, tpl32, &dd);
        if (dd < bd) { second = bd; bd = dd; best = STT_NPHRASE + s; }
        else if (dd < second) second = dd;
    }
    if (best < 0 || bd >= RECOG_THRESH) { stt_last_best = best; stt_last_dist = bd; return -1; }
    stt_last_best = best; stt_last_dist = bd;
    return best;
}

const char *stt_phrase_name(int idx) {
    if (idx >= STT_NPHRASE && idx < STT_NPHRASE + STT_MAX_SESS) {
        int s = idx - STT_NPHRASE;
        if (s < stt_sess_count && stt_sess_name[s][0]) return stt_sess_name[s];
    }
    if (idx < 0 || idx >= STT_NPHRASE) return "[Non ho capito. Riprova]";
    return stt_phrases[idx];
}

// ─── wrapper: cattura stereo 44.1k (dal driver AC97) → mono 16k → riconosci
// La cattura e' [frame][L,R] interleaved; il numero di frame validi e' n.

int stt_recognize_capture(const int16_t *stereo, int n) {
    int n16 = (int)((double)n * SR / 44100.0);
    if (n16 > MAX_FRAMES * HOP + NWIN) n16 = MAX_FRAMES * HOP + NWIN;
    double ratio = 44100.0 / (double)SR;
    for (int i = 0; i < n16; i++) {
        double src = i * ratio;
        int i0 = (int)src;
        if (i0 >= n - 1) i0 = n - 1;
        double frac = src - i0;
        double a = stereo[i0 * 2] / 32768.0;
        double b = stereo[(i0 + 1) * 2] / 32768.0;
        stt_mono[i] = (float)(a + frac * (b - a));
    }
    return stt_recognize_mono(stt_mono, n16);
}

// ─── risample stereo 44.1k → mono 16k in s16le (invio seriale al proxy) ───

int stt_resample_mono16(const int16_t *stereo, int n, int16_t *out, int max_out) {
    int n16 = (int)((double)n * SR / 44100.0);
    if (n16 > max_out) n16 = max_out;
    double ratio = 44100.0 / (double)SR;
    for (int i = 0; i < n16; i++) {
        double src = i * ratio;
        int i0 = (int)src;
        if (i0 >= n - 1) i0 = n - 1;
        double frac = src - i0;
        double a = stereo[i0 * 2];
        double b = stereo[(i0 + 1) * 2];
        out[i] = (int16_t)(a + frac * (b - a));
    }
    return n16;
}
