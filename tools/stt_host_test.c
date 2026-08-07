// Harness host per verificare il motore STT C contro il riferimento Python.
// Uso: stt_host_test <file.wav|file.pcm16k|file.aiff> [dump_mfcc.bin]
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define STT_HOST_TEST
#include "../kernel/drivers/audio/stt.c"

static int load_wav(const char *path, float **out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "aperto %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize);
    if (fread(buf, 1, fsize, f) != (size_t)fsize) { fprintf(stderr, "read fail\n"); return -1; }
    fclose(f);

    uint32_t rate = 16000, datalen = 0;
    uint16_t ch = 1, bps = 16;
    long off = 12;
    while (off + 8 <= fsize) {
        uint32_t csz = *(uint32_t *)(buf + off + 4);
        if (off + 8 + csz > fsize) break;
        if (!memcmp(buf + off, "fmt ", 4)) {
            rate = *(uint32_t *)(buf + off + 12);
            ch = *(uint16_t *)(buf + off + 10);
            bps = *(uint16_t *)(buf + off + 22);
        } else if (!memcmp(buf + off, "data", 4)) {
            datalen = csz;
            break;
        }
        off += 8 + csz + (csz & 1);
    }
    if (!datalen || off + 8 + datalen > fsize) { fprintf(stderr, "no data chunk\n"); return -1; }
    int16_t *raw = (int16_t *)(buf + off + 8);
    int ns = datalen / 2 / ch;
    float *sig = malloc(ns * sizeof(float));
    for (int i = 0; i < ns; i++) sig[i] = raw[i * ch] / 32768.0f;
    fprintf(stderr, "wav: rate=%u ch=%u bps=%u ns=%d\n", rate, ch, bps, ns);
    free(buf);
    *out = sig;
    return ns;
}

static int load_pcm16k(const char *path, float **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    int ns = sz / 2;
    float *sig = malloc(ns * sizeof(float));
    int16_t v;
    for (int i = 0; i < ns; i++) { fread(&v, 2, 1, f); sig[i] = v / 32768.0f; }
    fclose(f);
    *out = sig;
    return ns;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <audio>\n", argv[0]); return 2; }
    float *sig;
    int ns;
    const char *p = argv[1];
    int is_wav = strstr(p, ".wav") || strstr(p, ".aiff");
    if (is_wav) ns = load_wav(p, &sig);
    else ns = load_pcm16k(p, &sig);
    if (ns < 0) return 2;

    if (argc >= 3) {
        // dump MFCC per confronto con Python
        float feat[512 * 60];
        int nf = stt_mfcc(sig, ns, feat);
        FILE *f = fopen(argv[2], "wb");
        fwrite(&nf, 4, 1, f);
        fwrite(feat, sizeof(float), nf * 60, f);
        fclose(f);
        fprintf(stderr, "mfcc frames=%d dumped\n", nf);
    }

    int idx = stt_recognize_mono(sig, ns);
    if (idx >= 0) printf("RICONOSCIUTO: %s\n", stt_phrase_name(idx));
    else printf("NON RICONOSCIUTO\n");
    return 0;
}
