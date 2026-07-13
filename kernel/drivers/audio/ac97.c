#include "../pci/pci.c"
#include "../../lib/framebuffer.h"
#include "../../lib/io.h"

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
#define SAMPLE_MS   500
#define SAMPLE_COUNT (SAMPLE_RATE * SAMPLE_MS / 1000)
// Stereo: 2 int16 per sample (left+right), so buffer is 2x sample count
static int16_t sample_buf[SAMPLE_COUNT * 2] __attribute__((aligned(8)));

// ─── AC97 Initialization ───

void ac97_init(void)
{
    ac97_nabm = 0;
    ac97_mixer = 0;

    for (int i = 0; i < pci_count; i++) {
        unsigned int class = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 8);
        unsigned char base = (class >> 24) & 0xFF;
        unsigned char sub  = (class >> 16) & 0xFF;

        if (base == 0x04 && sub == 0x01) {
            unsigned int bar0 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x10);
            unsigned int bar1 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x14);
            ac97_nabm = bar0 & ~0xF;
            ac97_mixer = bar1 & ~0xF;

            // Enable PCI bus mastering + I/O space
            unsigned int cmd = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x04);
            cmd |= 0x05;
            pci_write32(pci_list[i].bus, pci_list[i].slot, 0, 0x04, cmd);

            break;
        }
    }

    if (!ac97_nabm || !ac97_mixer) {
        fb_write("AC97: no BARs\n");
        return;
    }

    // Reset mixer
    outw(ac97_mixer + MIX_RESET, 0);
    for (volatile int d = 0; d < 10000; d++);
    uint16_t reset_st = inw(ac97_mixer + MIX_RESET);
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

    fb_write("AC97 ok\n");

    ac97_initialized = 1;
}

int ac97_is_init(void) {
    return ac97_initialized;
}

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

void play_startup_melody(void)
{
    if (ac97_initialized) {
        ac97_play(NOTE_C4, 100);
        ac97_play(NOTE_E4, 100);
        ac97_play(NOTE_G4, 100);
        ac97_play(NOTE_C5, 200);
        return;
    }
    speaker_beep(NOTE_C4, 100);
    speaker_beep(NOTE_E4, 100);
    speaker_beep(NOTE_G4, 100);
    speaker_beep(NOTE_C5, 200);
    speaker_off();
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
