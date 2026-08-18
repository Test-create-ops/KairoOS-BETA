#include "../pci/pci.c"
#include "../../lib/framebuffer.h"
#include "../../lib/io.h"
#include "ac97_click.h"
#include "ac97_confirm.h"

// TEMP DEBUG (serial COM1, will be removed)
#define DPRINT(s) do { const char *_p = (s); while (*_p) { outb(0x3F8, *_p++); } } while (0)
static void dprint_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
}
static void dprint_hex(unsigned int v) {
    const char *h = "0123456789ABCDEF";
    outb(0x3F8, '0'); outb(0x3F8, 'x');
    for (int i = 7; i >= 0; i--) outb(0x3F8, h[(v >> (i * 4)) & 0xF]);
}
static void dprint_dec(int v) {
    char t[16]; int n = 0;
    if (v == 0) { outb(0x3F8, '0'); return; }
    while (v > 0 && n < 15) { t[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) outb(0x3F8, t[--n]);
}

// ─── AC97 registers ───

// NABM (Native Audio Bus Master) - BAR0
#define PO_BASE     0x10
#define PO_CIV      (PO_BASE + 0x00)
#define PO_LVI      (PO_BASE + 0x02)
#define PO_SR       (PO_BASE + 0x04)
#define PO_PICB     (PO_BASE + 0x06)
#define PO_PIV      (PO_BASE + 0x08)
#define PO_CR       (PO_BASE + 0x0A)
#define PO_BDBAR    (PO_BASE + 0x0C)

// PCM In (capture) registers
#define PI_BASE     0x20
#define PI_CIV      (PI_BASE + 0x00)
#define PI_LVI      (PI_BASE + 0x02)
#define PI_SR       (PI_BASE + 0x04)
#define PI_PICB     (PI_BASE + 0x06)
#define PI_PIV      (PI_BASE + 0x08)
#define PI_CR       (PI_BASE + 0x0A)
#define PI_BDBAR    (PI_BASE + 0x0C)

// PO/PI_CR bits
#define CR_RPBM     0x01
#define CR_RR       0x04
#define CR_LVBIE    0x08
#define CR_FEIE     0x10
#define CR_IOCE     0x20

// PO/PI_SR bits
#define SR_DCH      0x01
#define SR_BCIS     0x02
#define SR_LVBCI    0x04
#define SR_FIFOE    0x10

// Mixer - BAR1
#define MIX_RESET   0x00
#define MIX_MASTER  0x02
#define MIX_PCM     0x06
#define MIX_MIC     0x0E
#define MIX_REC_SELECT 0x1A
#define MIX_REC_GAIN   0x1C
#define MIX_RATE    0x2C

// Buffer descriptor
typedef struct {
    uint32_t pointer;
    uint16_t length;
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

#define BD_IOC 0x01
#define BD_BUP 0x02

// ─── Static state ───

static uint16_t ac97_nabm = 0;
static uint16_t ac97_mixer = 0;
static int ac97_initialized = 0;

// BD list (32 entries, 8 bytes each = 256 bytes) + sample buffer
// Aligned to 8-byte boundary for DMA
static ac97_bd_t bd_list[32] __attribute__((aligned(8)));
#define SAMPLE_RATE 44100
#define SAMPLE_MS   1500
#define SAMPLE_COUNT (SAMPLE_RATE * SAMPLE_MS / 1000)
// Stereo: 2 int16 per sample (left+right), so buffer is 2x sample count
static int16_t sample_buf[SAMPLE_COUNT * 2] __attribute__((aligned(8)));

// ─── AC97 Initialization ───

void ac97_init(void)
{
    ac97_nabm = 0;
    ac97_mixer = 0;

    dprint_init();
    DPRINT("\r\n[ac97] init pci_count="); dprint_dec(pci_count); DPRINT("\r\n");

    for (int i = 0; i < pci_count; i++) {
        unsigned int class = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 8);
        unsigned char base = (class >> 24) & 0xFF;
        unsigned char sub  = (class >> 16) & 0xFF;

        DPRINT("[ac97] dev "); dprint_dec(i); DPRINT(" base=0x"); dprint_hex(base); DPRINT(" sub=0x"); dprint_hex(sub); DPRINT("\r\n");

        if (base == 0x04 && sub == 0x01) {
            unsigned int bar0 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x10);
            unsigned int bar1 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x14);
            ac97_nabm = bar0 & ~0xF;
            ac97_mixer = bar1 & ~0xF;

            // Enable PCI bus mastering + I/O space
            unsigned int cmd = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x04);
            cmd |= 0x05;
            pci_write32(pci_list[i].bus, pci_list[i].slot, 0, 0x04, cmd);

            DPRINT("[ac97] FOUND bar0=0x"); dprint_hex(bar0); DPRINT(" bar1=0x"); dprint_hex(bar1); DPRINT("\r\n");
            break;
        }
    }

    if (!ac97_nabm || !ac97_mixer) {
        DPRINT("[ac97] NO BARS\r\n");
        fb_write("AC97: no BARs\n");
        return;
    }

    // Reset mixer
    outw(ac97_mixer + MIX_RESET, 0);
    for (volatile int d = 0; d < 10000; d++);
    uint16_t reset_st = inw(ac97_mixer + MIX_RESET);
    DPRINT("[ac97] mixer reset_st=0x"); dprint_hex(reset_st); DPRINT("\r\n");
    if (reset_st & 0x01) {
        fb_write("AC97 mixer ready\n");
    } else {
        fb_write("AC97 mixer no ready\n");
    }

    // Unmute + set master volume to max
    outw(ac97_mixer + MIX_MASTER, 0x1F1F);
    outw(ac97_mixer + MIX_PCM, 0x1F1F);

    // Set sample rate to 44100 Hz
    outw(ac97_mixer + MIX_RATE, SAMPLE_RATE);
    for (volatile int d = 0; d < 1000; d++);
    uint16_t rate = inw(ac97_mixer + MIX_RATE);
    DPRINT("[ac97] rate written, readback=0x"); dprint_hex(rate); DPRINT("\r\n");

    fb_write("AC97 ok\n");

    ac97_initialized = 1;
    DPRINT("[ac97] initialized OK\r\n");
}

int ac97_is_init(void) {
    return ac97_initialized;
}

static int current_volume = 31;
static int current_mute = 0;

void ac97_set_volume(int level) {
    if (!ac97_initialized) return;
    if (level < 0) level = 0;
    if (level > 31) level = 31;
    current_volume = level;
    uint16_t mute_bits = current_mute ? ((1 << 5) | (1 << 13)) : 0;
    uint16_t val = ((level << 8) | level) | mute_bits;
    outw(ac97_mixer + MIX_MASTER, val);
    outw(ac97_mixer + MIX_PCM, val);
}

void ac97_set_mute(int mute) {
    if (!ac97_initialized) return;
    current_mute = mute;
    uint16_t mute_bits = mute ? ((1 << 5) | (1 << 13)) : 0;
    uint16_t val = ((current_volume << 8) | current_volume) | mute_bits;
    outw(ac97_mixer + MIX_MASTER, val);
}

int ac97_get_volume(void) { return current_volume; }
int ac97_get_mute(void) { return current_mute; }

// ─── Play a tone via AC97 DMA ───

void ac97_play(uint32_t freq, uint32_t ms)
{
    if (!ac97_initialized) return;

    // Reset PCM Out channel
    outw(ac97_nabm + PO_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);

    // Generate square wave samples
    int nsamples = SAMPLE_RATE * ms / 1000;
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    if (nsamples < 64) nsamples = 64;

    int half_period = SAMPLE_RATE / (freq * 2);
    if (half_period < 1) half_period = 1;

    // Generate stereo square wave (left + right interleaved)
    for (int i = 0; i < nsamples; i++) {
        int16_t v = ((i / half_period) & 1) ? 16000 : -16000;
        sample_buf[i * 2] = v;      // left
        sample_buf[i * 2 + 1] = v;  // right
    }

    // Set up one buffer descriptor: 4 bytes per stereo frame
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC;

    // Write BD list base address
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 1000; d++);

    // Set Last Valid Index to 0 (just one BD)
    outw(ac97_nabm + PO_LVI, 0);
    for (volatile int d = 0; d < 1000; d++);

    // Start playback
    outw(ac97_nabm + PO_CR, CR_RPBM);

    // Wait for audio duration (busy-wait roughly matching ms)
    for (volatile uint32_t w = 0; w < ms * 20000; w++) {
        asm volatile("pause");
        uint16_t sr = inw(ac97_nabm + PO_SR);
        if (sr & SR_DCH) break;
    }

    // Stop
    outw(ac97_nabm + PO_CR, 0);
}

// ─── Non-blocking playback + background music (original tune) ─────────

void ac97_play_nb(uint32_t freq, uint32_t ms, int amp)
{
    DPRINT("[ac97] play_nb f="); dprint_dec(freq); DPRINT("\r\n");
    if (!ac97_initialized) return;

    outw(ac97_nabm + PO_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);

    int nsamples = SAMPLE_RATE * ms / 1000;
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    if (nsamples < 64) nsamples = 64;

    if (freq > 0) {
        int half_period = SAMPLE_RATE / (freq * 2);
        if (half_period < 1) half_period = 1;
        for (int i = 0; i < nsamples; i++) {
            int16_t v = ((i / half_period) & 1) ? (int16_t)amp : (int16_t)(-amp);
            sample_buf[i * 2] = v;
            sample_buf[i * 2 + 1] = v;
        }
    } else {
        for (int i = 0; i < nsamples; i++) { sample_buf[i * 2] = 0; sample_buf[i * 2 + 1] = 0; }
    }

    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
}

int ac97_busy(void)
{
    if (!ac97_initialized) return 0;
    return !(inw(ac97_nabm + PO_SR) & SR_DCH);
}

// ─── Play raw PCM samples (non-blocking, for VoIP) ───
void ac97_play_raw(const int16_t *samples, int count)
{
    if (!ac97_initialized || count <= 0) return;
    if (count > SAMPLE_COUNT) count = SAMPLE_COUNT;

    outw(ac97_nabm + PO_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);

    for (int i = 0; i < count; i++) {
        sample_buf[i * 2] = samples[i];     // left
        sample_buf[i * 2 + 1] = samples[i]; // right
    }

    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = count * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
}

// ─── Keyboard click ───

void ac97_play_click(void)
{
    if (!ac97_initialized) return;
    int nsamples = CLICK_PCM_LEN;
    if (nsamples > SAMPLE_COUNT * 2) nsamples = SAMPLE_COUNT * 2;
    for (int i = 0; i < nsamples; i++)
        sample_buf[i] = click_pcm[i];
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 2;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    int wait_us = (int)((unsigned long)CLICK_NUM_FRAMES * 1000000UL / SAMPLE_RATE);
    for (volatile int w = 0; w < wait_us; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// ─── Confirm button sound (Wii-style) ───

void ac97_play_confirm(void)
{
    if (!ac97_initialized) return;
    int nsamples = CONFIRM_PCM_LEN;
    if (nsamples > SAMPLE_COUNT * 2) nsamples = SAMPLE_COUNT * 2;
    for (int i = 0; i < nsamples; i++)
        sample_buf[i] = confirm_pcm[i];
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 2;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    int wait_us = (int)((unsigned long)CONFIRM_NUM_FRAMES * 1000000UL / SAMPLE_RATE);
    for (volatile int w = 0; w < wait_us; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// ─── PC Speaker (legacy) — kept for compatibility ───

static void speaker_off(void)
{
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & 0xFC);
}

void speaker_tone(uint32_t freq)
{
    if (freq < 20 || freq > 20000) { speaker_off(); return; }
    uint32_t div = 1193182 / freq;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, (div >> 8) & 0xFF);
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 3);
}

void speaker_beep(uint32_t freq, uint32_t ms)
{
    speaker_tone(freq);
    for (volatile uint32_t i = 0; i < ms * 3000; i++);
    speaker_off();
}

// Note frequencies
#define NOTE_B0  31
#define NOTE_C1  33
#define NOTE_CS1 35
#define NOTE_D1  37
#define NOTE_DS1 39
#define NOTE_E1  41
#define NOTE_F1  44
#define NOTE_FS1 46
#define NOTE_G1  49
#define NOTE_GS1 52
#define NOTE_A1  55
#define NOTE_AS1 58
#define NOTE_B1  62
#define NOTE_C2  65
#define NOTE_CS2 69
#define NOTE_D2  73
#define NOTE_DS2 78
#define NOTE_E2  82
#define NOTE_F2  87
#define NOTE_FS2 93
#define NOTE_G2  98
#define NOTE_GS2 104
#define NOTE_A2  110
#define NOTE_AS2 117
#define NOTE_B2  123
#define NOTE_C3  131
#define NOTE_CS3 139
#define NOTE_D3  147
#define NOTE_DS3 156
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_FS3 185
#define NOTE_G3  196
#define NOTE_GS3 208
#define NOTE_A3  220
#define NOTE_AS3 233
#define NOTE_B3  247
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_CS6 1109
#define NOTE_D6  1175
#define NOTE_DS6 1245
#define NOTE_E6  1319
#define NOTE_F6  1397
#define NOTE_FS6 1480
#define NOTE_G6  1568

// ─── Original chiptune for the setup wizard (100% original, no copyright) ──
// Andamento allegro in Do maggiore. Nota 0 = pausa. Durate in millisecondi.

#define MUSIC_LEN 64
static const uint32_t music_freq[MUSIC_LEN] = {
    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5, NOTE_F5, NOTE_A5, NOTE_C6, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_G5, NOTE_E5, NOTE_D5, NOTE_F5, NOTE_A5, NOTE_F5,
    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_C6, NOTE_B5, NOTE_G5, NOTE_D5, NOTE_G5,
    NOTE_A5, NOTE_G5, NOTE_E5, NOTE_D5, NOTE_C5, 0,      NOTE_C5, 0,
    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5, NOTE_F5, NOTE_A5, NOTE_C6, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_G5, NOTE_E5, NOTE_D5, NOTE_F5, NOTE_A5, NOTE_F5,
    NOTE_E5, NOTE_G5, NOTE_C6, NOTE_G5, NOTE_B5, NOTE_G5, NOTE_D5, NOTE_B5,
    NOTE_A5, NOTE_G5, NOTE_F5, NOTE_E5, NOTE_D5, NOTE_C5, 0,      0,
};
static const uint32_t music_ms[MUSIC_LEN] = {
    180,180,180,180, 180,180,180,180, 180,180,180,180, 180,180,180,180,
    180,180,180,240, 180,180,180,180, 180,180,180,180, 240,120, 240,120,
    180,180,180,180, 180,180,180,180, 180,180,180,180, 180,180,180,180,
    180,180,180,180, 180,180,180,180, 180,180,180,180, 180,240, 240,120,
};

static int music_on = 0;
static int music_i = 0;

void ac97_music_start(void)
{
    if (!ac97_initialized) return;
    music_on = 1;
    music_i = 0;
    ac97_play_nb(music_freq[0], music_ms[0], 9000);
}

void ac97_music_stop(void)
{
    music_on = 0;
    if (ac97_initialized) outw(ac97_nabm + PO_CR, 0);
}

// Chiamato a ogni frame: quando la nota corrente finisce, parte la prossima.
void ac97_music_poll(void)
{
    if (!music_on || !ac97_initialized) return;
    if (ac97_busy()) return;
    music_i++;
    if (music_i >= MUSIC_LEN) music_i = 0;
    ac97_play_nb(music_freq[music_i], music_ms[music_i], 9000);
}

void play_startup_melody(void)
{
    // Original "power-up" chime (no copyrighted melody)
    if (ac97_initialized) {
        ac97_play(NOTE_G5, 90);
        ac97_play(NOTE_C6, 90);
        ac97_play(NOTE_E6, 90);
        ac97_play(NOTE_G6, 260);
        ac97_play(NOTE_C6, 120);
        ac97_play(NOTE_G5, 180);
        return;
    }
    speaker_beep(NOTE_G5, 90);
    speaker_beep(NOTE_C6, 90);
    speaker_beep(NOTE_E6, 90);
    speaker_beep(NOTE_G6, 260);
    speaker_beep(NOTE_C6, 120);
    speaker_beep(NOTE_G5, 180);
    speaker_off();
}

// ─── Ambient background music (slow, calm loop) ───
#define AMB_LEN 48
static const uint32_t amb_freq[AMB_LEN] = {
    NOTE_C4, 0, NOTE_E4, 0, NOTE_G4, 0, NOTE_C5, 0,
    NOTE_B4, 0, NOTE_G4, 0, NOTE_E4, 0, NOTE_C4, 0,
    NOTE_F4, 0, NOTE_A4, 0, NOTE_C5, 0, NOTE_A4, 0,
    NOTE_G4, 0, NOTE_E4, 0, NOTE_D4, 0, NOTE_C4, 0,
    NOTE_A3, 0, NOTE_C4, 0, NOTE_E4, 0, NOTE_A4, 0,
    NOTE_G4, 0, NOTE_E4, 0, NOTE_C4, 0, 0, 0,
};
static const uint32_t amb_ms[AMB_LEN] = {
    400,100,400,100,400,100,600,100,
    400,100,400,100,400,100,600,100,
    400,100,400,100,400,100,600,100,
    400,100,400,100,400,100,600,100,
    400,100,400,100,400,100,600,100,
    400,100,400,100,600,100,800,400,
};

static int amb_on = 0;
static int amb_i = 0;

void ac97_ambient_start(void)
{
    if (!ac97_initialized) return;
    amb_on = 1;
    amb_i = 0;
    ac97_play_nb(amb_freq[0], amb_ms[0], 3000);
}

void ac97_ambient_stop(void)
{
    amb_on = 0;
    if (ac97_initialized) outw(ac97_nabm + PO_CR, 0);
}

void ac97_ambient_poll(void)
{
    if (!amb_on || !ac97_initialized) return;
    if (ac97_busy()) return;
    amb_i++;
    if (amb_i >= AMB_LEN) { amb_on = 0; return; }
    if (amb_freq[amb_i] == 0) { ac97_play_nb(0, amb_ms[amb_i], 1000); }
    else { ac97_play_nb(amb_freq[amb_i], amb_ms[amb_i], 3000); }
}

void play_sweep(uint32_t freq_start, uint32_t freq_end, uint32_t ms)
{
    if (ac97_initialized) {
        ac97_play(freq_start, ms);
        return;
    }
    uint32_t steps = 50;
    for (uint32_t s = 0; s < steps; s++) {
        uint32_t freq = freq_start + (freq_end - freq_start) * s / steps;
        speaker_tone(freq);
        for (volatile uint32_t i = 0; i < ms * 60 / steps; i++);
    }
    speaker_off();
}

void play_noise(uint32_t ms)
{
    if (ac97_initialized) {
        ac97_play(200, ms);
        return;
    }
    for (volatile uint32_t t = 0; t < ms * 200; t++) {
        uint32_t freq = 200 + (t * 137 + 53) % 500;
        speaker_tone(freq);
    }
    speaker_off();
}

// ─── Microphone Capture ───

static int16_t capture_buf[SAMPLE_COUNT * 2] __attribute__((aligned(8)));
static ac97_bd_t cap_bd_list[32] __attribute__((aligned(8)));
static int capture_active = 0;

void ac97_start_capture(void)
{
    if (!ac97_initialized) return;
    outw(ac97_nabm + PI_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);
    cap_bd_list[0].pointer = (uint32_t)(uintptr_t)capture_buf;
    cap_bd_list[0].length = SAMPLE_COUNT * 4;
    cap_bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PI_BDBAR, (uint32_t)(uintptr_t)cap_bd_list);
    outw(ac97_nabm + PI_LVI, 0);
    for (volatile int d = 0; d < 1000; d++);
    outw(ac97_mixer + MIX_REC_SELECT, 0x0000);
    outw(ac97_mixer + MIX_REC_GAIN, 0x0F0F);
    outw(ac97_nabm + PI_CR, CR_RPBM);
    capture_active = 1;
}

int ac97_capture_level(void)
{
    if (!capture_active) return 0;
    int peak = 0, ns = 50;
    if (ns > SAMPLE_COUNT) ns = SAMPLE_COUNT;
    for (int i = 0; i < ns; i++) {
        int s = capture_buf[i*2];
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    return peak;
}

int ac97_capture_is_active(void) { return capture_active; }

void ac97_stop_capture(void)
{
    if (!capture_active) return;
    outw(ac97_nabm + PI_CR, 0);
    capture_active = 0;
}

// Read stereo samples from capture buffer at given offset
void ac97_capture_read(int16_t *out, int offset, int count)
{
    if (!capture_active) { for (int i = 0; i < count*2; i++) out[i] = 0; return; }
    for (int i = 0; i < count; i++) {
        int idx = (offset + i) % SAMPLE_COUNT;
        out[i*2]     = capture_buf[idx*2];     // left
        out[i*2 + 1] = capture_buf[idx*2 + 1]; // right
    }
}

// Get current DMA write position (for tracking)
int ac97_capture_pos(void)
{
    if (!capture_active) return 0;
    uint16_t pos = inw(ac97_nabm + PI_CIV);
    return (int)pos;
}

// ─── STT capture (ring 10s, riconoscimento in-kernel) ────────────────────

#define STT_CAP_SECS  5
#define STT_CAP_FRAMES (44100 * STT_CAP_SECS)
#define STT_CAP_BYTES  (STT_CAP_FRAMES * 4)   // 4 byte per frame stereo

static int16_t stt_cap_buf[STT_CAP_FRAMES * 2] __attribute__((aligned(16)));
static ac97_bd_t stt_cap_bd[32] __attribute__((aligned(8)));
static int stt_cap_active = 0;
static int stt_cap_pos = 0;   // ultima posizione DMA osservata (bytes)

void ac97_stt_start(void)
{
    if (!ac97_initialized) return;
    outw(ac97_nabm + PI_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);
    for (volatile int *p = (volatile int *)stt_cap_buf;
         p < (volatile int *)(stt_cap_buf + STT_CAP_FRAMES * 2); p++) *p = 0;
    stt_cap_bd[0].pointer = (uint32_t)(uintptr_t)stt_cap_buf;
    stt_cap_bd[0].length = STT_CAP_BYTES;
    stt_cap_bd[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PI_BDBAR, (uint32_t)(uintptr_t)stt_cap_bd);
    outw(ac97_nabm + PI_LVI, 0);
    for (volatile int d = 0; d < 1000; d++);
    outw(ac97_mixer + MIX_REC_SELECT, 0x0000);   // fonte = microfono
    outw(ac97_mixer + MIX_REC_GAIN, 0x0F0F);     // guadagno max
    outw(ac97_mixer + 0x32, 44100);              // PCM ADC rate
    outw(ac97_nabm + PI_CR, CR_RPBM);
    stt_cap_active = 1;
    stt_cap_pos = 0;
}

int ac97_stt_active(void) { return stt_cap_active; }

// Da chiamare periodicamente; tiene traccia della posizione DMA massima.
void ac97_stt_track(void)
{
    if (!stt_cap_active) return;
    uint16_t picb = inw(ac97_nabm + PI_PICB);
    long pos = STT_CAP_BYTES - picb;
    if (pos < 0) pos = 0;
    if (pos > STT_CAP_BYTES) pos = STT_CAP_BYTES;
    if (pos > stt_cap_pos) stt_cap_pos = (int)pos;
}

int ac97_stt_valid_frames(void)
{
    ac97_stt_track();
    if (stt_cap_pos <= 0) return 0;
    int frames = stt_cap_pos / 4;
    if (frames > STT_CAP_FRAMES) frames = STT_CAP_FRAMES;
    return frames;
}

const int16_t *ac97_stt_buffer(void) { return stt_cap_buf; }

// Livello istantaneo del microfono (picco degli ultimi campioni catturati)
int ac97_stt_level(void)
{
    if (!stt_cap_active) return 0;
    int peak = 0, n = 2000;
    if (n > STT_CAP_FRAMES * 2) n = STT_CAP_FRAMES * 2;
    int start = stt_cap_pos / 2;           // byte -> campioni
    if (start + n > STT_CAP_FRAMES * 2) start = STT_CAP_FRAMES * 2 - n;
    if (start < 0) start = 0;
    for (int i = 0; i < n; i += 2) {
        int s = stt_cap_buf[start + i];
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    return peak;
}

void ac97_stt_stop(void)
{
    if (!stt_cap_active) return;
    ac97_stt_track();
    outw(ac97_nabm + PI_CR, 0);
    stt_cap_active = 0;
}

// ─── Procedural UI sounds (generated on-the-fly, no PCM arrays) ────────

// Soft dock hover tick — very short, gentle sine blip
void ac97_play_hover(void)
{
    if (!ac97_initialized) return;
    int nsamples = SAMPLE_RATE * 30 / 1000; // 30ms
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    int freq = 2400;
    int half_period = SAMPLE_RATE / (freq * 2);
    if (half_period < 1) half_period = 1;
    int16_t peak = 4000;
    for (int i = 0; i < nsamples; i++) {
        int16_t v = ((i / half_period) & 1) ? peak : -peak;
        int16_t env = peak - (peak * i / nsamples);
        v = v * env / peak;
        sample_buf[i * 2] = v;
        sample_buf[i * 2 + 1] = v;
    }
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    for (volatile int w = 0; w < 30000; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// Notification chime — two-tone ascending (like macOS)
void ac97_play_notify(void)
{
    if (!ac97_initialized) return;
    int nsamples = SAMPLE_RATE * 300 / 1000; // 300ms
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    int freqs[2] = {880, 1320};
    int tones[2] = {150, 150};
    int pos = 0;
    for (int t = 0; t < 2; t++) {
        int half_period = SAMPLE_RATE / (freqs[t] * 2);
        if (half_period < 1) half_period = 1;
        int16_t peak = 12000;
        for (int i = 0; i < tones[t] && pos < nsamples; i++, pos++) {
            int16_t env = peak - (peak * i / tones[t]);
            int16_t v = ((pos / half_period) & 1) ? peak : -peak;
            v = v * env / peak;
            sample_buf[pos * 2] = v;
            sample_buf[pos * 2 + 1] = v;
        }
    }
    // Fill remaining with silence
    for (; pos < nsamples; pos++) {
        sample_buf[pos * 2] = 0;
        sample_buf[pos * 2 + 1] = 0;
    }
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    for (volatile int w = 0; w < 300000; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// Startup chime — 3-note ascending with fade-in/out (800ms)
void ac97_play_startup(void)
{
    if (!ac97_initialized) return;
    int nsamples = SAMPLE_RATE * 800 / 1000; // 800ms
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    int freqs[3] = {523, 659, 784}; // C5, E5, G5 major chord arpeggio
    int note_len = nsamples / 4;
    for (int i = 0; i < nsamples; i++) {
        int note = i / note_len;
        if (note > 2) note = 2;
        int16_t v = 0;
        for (int h = 1; h <= 3; h++) {
            int f = freqs[note] * h;
            int hp = SAMPLE_RATE / (f * 2);
            if (hp < 1) hp = 1;
            int amp = 6000 / h;
            v += ((i / hp) & 1) ? amp : -amp;
        }
        // Envelope: fade in 50ms, sustain, fade out 100ms
        int16_t env = 1000;
        int fade_in = SAMPLE_RATE * 50 / 1000;
        int fade_out = SAMPLE_RATE * 100 / 1000;
        if (i < fade_in) env = 1000 * i / fade_in;
        else if (i > nsamples - fade_out) env = 1000 * (nsamples - i) / fade_out;
        v = v * env / 1000;
        sample_buf[i * 2] = v;
        sample_buf[i * 2 + 1] = v;
    }
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    for (volatile int w = 0; w < 800000; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// Error buzz — low harsh buzz for errors
void ac97_play_error(void)
{
    if (!ac97_initialized) return;
    int nsamples = SAMPLE_RATE * 200 / 1000; // 200ms
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    int freq = 120;
    int half_period = SAMPLE_RATE / (freq * 2);
    if (half_period < 1) half_period = 1;
    int16_t peak = 8000;
    for (int i = 0; i < nsamples; i++) {
        int16_t v = ((i / half_period) & 1) ? peak : -peak;
        int16_t env = peak - (peak * i / nsamples);
        v = v * env / peak;
        sample_buf[i * 2] = v;
        sample_buf[i * 2 + 1] = v;
    }
    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
    for (volatile int w = 0; w < 200000; w++) {
        asm volatile("pause");
        if (inw(ac97_nabm + PO_SR) & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// ─── DTMF dual-tone generation (Phone dial pad) ─────────────────────

void ac97_play_dtmf(char digit, uint32_t ms)
{
    if (!ac97_initialized) return;

    // DTMF frequency table: rows (low group) and columns (high group)
    //        1209Hz  1336Hz  1477Hz  1633Hz
    // 697Hz:   1       2       3       A
    // 770Hz:   4       5       6       B
    // 852Hz:   7       8       9       C
    // 941Hz:   *       0       #       D
    static const int dtmf_rows[] = {697, 770, 852, 941};
    static const int dtmf_cols[] = {1209, 1336, 1477, 1633};
    // Map digit to (row, col) index
    static const int dtmf_map[][2] = {
        {3,1}, // 0
        {0,0}, // 1
        {0,1}, // 2
        {0,2}, // 3
        {1,0}, // 4
        {1,1}, // 5
        {1,2}, // 6
        {2,0}, // 7
        {2,1}, // 8
        {2,2}, // 9
        {3,0}, // *
        {3,2}, // #
    };

    int idx = -1;
    if (digit >= '0' && digit <= '9') idx = digit - '0';
    else if (digit == '*') idx = 10;
    else if (digit == '#') idx = 11;
    if (idx < 0) return;

    int row_freq = dtmf_rows[dtmf_map[idx][0]];
    int col_freq = dtmf_cols[dtmf_map[idx][1]];

    outw(ac97_nabm + PO_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);

    int nsamples = SAMPLE_RATE * ms / 1000;
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    if (nsamples < 64) nsamples = 64;

    // Generate dual-tone: sum of two sine waves
    // Using integer approximation of sine (parabolic)
    for (int i = 0; i < nsamples; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        // Row tone + column tone, mixed at half amplitude each
        float row_sin = k_cosf(2.0f * 3.14159f * row_freq * t - 1.5708f); // sin via cos(pi/2 shift)
        float col_sin = k_cosf(2.0f * 3.14159f * col_freq * t - 1.5708f);
        float mixed = (row_sin + col_sin) * 0.35f; // scale to avoid clipping
        int16_t v = (int16_t)(mixed * 16000.0f);
        sample_buf[i * 2] = v;
        sample_buf[i * 2 + 1] = v;
    }

    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 1000; d++);
    outw(ac97_nabm + PO_LVI, 0);
    for (volatile int d = 0; d < 1000; d++);
    outw(ac97_nabm + PO_CR, CR_RPBM);

    // Wait for duration
    for (volatile uint32_t w = 0; w < ms * 20000; w++) {
        asm volatile("pause");
        uint16_t sr = inw(ac97_nabm + PO_SR);
        if (sr & SR_DCH) break;
    }
    outw(ac97_nabm + PO_CR, 0);
}

// ─── DTMF dual-tone non-blocking (for ambient phone ring) ───
void ac97_play_dtmf_nb(char digit)
{
    if (!ac97_initialized) return;

    static const int dtmf_rows[] = {697, 770, 852, 941};
    static const int dtmf_cols[] = {1209, 1336, 1477, 1633};
    static const int dtmf_map[][2] = {
        {3,1},{0,0},{0,1},{0,2},{1,0},{1,1},{1,2},{2,0},{2,1},{2,2},{3,0},{3,2}
    };

    int idx = -1;
    if (digit >= '0' && digit <= '9') idx = digit - '0';
    else if (digit == '*') idx = 10;
    else if (digit == '#') idx = 11;
    if (idx < 0) return;

    int row_freq = dtmf_rows[dtmf_map[idx][0]];
    int col_freq = dtmf_cols[dtmf_map[idx][1]];

    outw(ac97_nabm + PO_CR, CR_RR);
    for (volatile int d = 0; d < 1000; d++);

    int nsamples = SAMPLE_RATE * 200 / 1000; // 200ms
    if (nsamples > SAMPLE_COUNT) nsamples = SAMPLE_COUNT;
    if (nsamples < 64) nsamples = 64;

    for (int i = 0; i < nsamples; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        float row_sin = k_cosf(2.0f * 3.14159f * row_freq * t - 1.5708f);
        float col_sin = k_cosf(2.0f * 3.14159f * col_freq * t - 1.5708f);
        float mixed = (row_sin + col_sin) * 0.35f;
        int16_t v = (int16_t)(mixed * 16000.0f);
        sample_buf[i * 2] = v;
        sample_buf[i * 2 + 1] = v;
    }

    bd_list[0].pointer = (uint32_t)(uintptr_t)sample_buf;
    bd_list[0].length = nsamples * 4;
    bd_list[0].flags = BD_IOC | BD_BUP;
    outl(ac97_nabm + PO_BDBAR, (uint32_t)(uintptr_t)bd_list);
    for (volatile int d = 0; d < 500; d++);
    outw(ac97_nabm + PO_LVI, 0);
    outw(ac97_nabm + PO_CR, CR_RPBM);
}
