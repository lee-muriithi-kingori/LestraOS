/*
 * Lestra OS - AC97 Microphone Capture (PCM-in)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Real AC97 PCM-in capture driver. The existing ac97.c only handles
 * PCM-out (playback); this file adds PCM-in (recording) so the STT
 * engine can actually read microphone samples.
 *
 * AC97 register layout (NABM BAR0):
 *   0x00  PCM_IN_BD_LIST   (BD list address, 16-byte aligned)
 *   0x04  PCM_IN_CIV       (current index value, RO)
 *   0x05  PCM_IN_LVI       (last valid index)
 *   0x06  PCM_IN_SR        (status register)
 *   0x08  PCM_IN_PICB      (position in current buffer, RO)
 *   0x09  PCM_IN_PIV       (prefetched index value)
 *   0x0B  PCM_IN_CR        (control register)
 *
 * Control register bits:
 *   bit 0  RPA  - run/pause (1 = pause after current buffer)
 *   bit 1  RPBM - run/pause bus master (1 = run)
 *   bit 2  RIRQ - reset interrupts
 *   bit 3  LVBIE- last valid buffer interrupt enable
 *   bit 4  CVIE - current valid interrupt enable
 *   bit 5  FEIE - fifo error interrupt enable
 *   bit 6  IOCE - interrupt on completion enable
 *
 * We use the same BD-list scheme as playback: 8 × 4 KB buffers, the
 * hardware cycles through them, we poll SR for completion.
 *
 * AC97 NAM (BAR1) mixer:
 *   0x00  reset
 *   0x0A  mic volume (record level)
 *   0x1A  record gain
 *   0x1C  record select (which input source: 0 = mic, 1 = CD, etc.)
 *
 * We configure: record source = mic (0), record gain = max (0x0F0F).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/pci.h>
#include <string.h>

/* NABM PCM-in register offsets */
#define NABM_PCM_IN_BD_LIST  0x00
#define NABM_PCM_IN_CIV      0x04
#define NABM_PCM_IN_LVI      0x05
#define NABM_PCM_IN_SR       0x06
#define NABM_PCM_IN_PICB     0x08
#define NABM_PCM_IN_PIV      0x09
#define NABM_PCM_IN_CR       0x0B

/* NABM global */
#define NABM_GLOBAL_CTRL     0x2C
#define NABM_GLOBAL_STATUS   0x30

/* Control register bits */
#define CR_RPA   0x01
#define CR_RPBM  0x02
#define CR_RIRQ  0x04
#define CR_LVBIE 0x08
#define CR_CVIE  0x10
#define CR_FEIE  0x20
#define CR_IOCE  0x40

/* Status register bits */
#define SR_DCH   0x01   /* DMA controller halted */
#define SR_CELV  0x02   /* current equals last valid */
#define SR_LVBCI 0x04   /* last valid buffer completion interrupt */
#define SR_BCIS  0x08   /* buffer completion interrupt status */
#define SR_FIFOE 0x10   /* FIFO error */

struct bd_entry {
    uint32_t addr;
    uint16_t length;   /* in samples (16-bit stereo = 2 samples per dword) */
    uint16_t ctrl;
} __packed;

#define NUM_CAP_BUFFERS 8
#define CAP_BUFFER_SIZE 4096   /* 4 KB per buffer = 1024 stereo samples = ~23 ms @ 44100 Hz */

/* Reuse the NABM base from ac97.c. We re-discover the PCI device here
 * to keep this file self-contained. */
static uintptr_t cap_nabm_base = 0;
static uintptr_t cap_nam_base  = 0;
static int cap_inited = 0;

/* Per-buffer BD list and capture buffers (16-byte aligned). */
static struct bd_entry cap_bd_list[NUM_CAP_BUFFERS] __aligned(16);
static uint8_t cap_buffers[NUM_CAP_BUFFERS][CAP_BUFFER_SIZE] __aligned(128);

/* Ring buffer that holds captured samples for the STT engine to drain. */
#define CAP_RING_BYTES (64 * 1024)   /* 64 KB ring = ~0.7 s of audio at 44100 Hz stereo 16-bit */
static uint8_t cap_ring[CAP_RING_BYTES];
static volatile uint32_t cap_ring_head = 0;   /* writer (IRQ / poll) */
static volatile uint32_t cap_ring_tail = 0;   /* reader (STT engine) */

static inline uint8_t  cap_nabm_read8 (uint32_t off) { return inb(cap_nabm_base + off); }
static inline uint16_t cap_nabm_read16(uint32_t off) { return inw(cap_nabm_base + off); }
static inline uint32_t cap_nabm_read32(uint32_t off) { return inl(cap_nabm_base + off); }
static inline void cap_nabm_write8 (uint32_t off, uint8_t  v) { outb(cap_nabm_base + off, v); }
static inline void cap_nabm_write16(uint32_t off, uint16_t v) { outw(cap_nabm_base + off, v); }
static inline void cap_nabm_write32(uint32_t off, uint32_t v) { outl(cap_nabm_base + off, v); }

static inline uint16_t cap_nam_read16(uint8_t offset) {
    outw(cap_nam_base + 0x04, offset);
    return inw(cap_nam_base + 0x06);
}
static inline void cap_nam_write16(uint8_t offset, uint16_t val) {
    outw(cap_nam_base + 0x04, offset);
    outw(cap_nam_base + 0x06, val);
}

static int cap_find_ac97(uint8_t* bus, uint8_t* dev, uint8_t* func) {
    for (uint8_t d = 0; d < 32; d++) {
        for (uint8_t f = 0; f < 8; f++) {
            uint32_t id = pci_config_read32(0, d, f, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint32_t class_code = pci_config_read32(0, d, f, 0x08);
            uint8_t base_class = (class_code >> 24) & 0xFF;
            uint8_t subclass   = (class_code >> 16) & 0xFF;
            if (base_class == 0x04 && subclass == 0x01) {
                *bus = 0; *dev = d; *func = f;
                return 1;
            }
        }
    }
    return 0;
}

/* Public: initialize PCM-in. Returns 1 if capture is available. */
int ac97_capture_init(void) {
    if (cap_inited) return 1;

    uint8_t bus, dev, func;
    if (!cap_find_ac97(&bus, &dev, &func)) {
        pr_info("ac97-cap: no AC97 controller found\n");
        return 0;
    }

    uint32_t bar0 = pci_config_read32(bus, dev, func, 0x10);
    uint32_t bar1 = pci_config_read32(bus, dev, func, 0x14);
    cap_nabm_base = bar0 & ~0xFu;
    cap_nam_base  = bar1 & ~0xFu;

    pr_info("ac97-cap: NABM=0x%x NAM=0x%x\n",
            (unsigned)cap_nabm_base, (unsigned)cap_nam_base);

    /* Reset PCM-in channel: pulse RPA, wait for it to clear. */
    cap_nabm_write8(NABM_PCM_IN_CR, CR_RPA);
    int timeout = 10000;
    while ((cap_nabm_read8(NABM_PCM_IN_CR) & CR_RPA) && timeout-- > 0);

    /* Set up BD list. Each entry points to one of our 4 KB buffers. */
    for (int i = 0; i < NUM_CAP_BUFFERS; i++) {
        cap_bd_list[i].addr   = (uint32_t)(uintptr_t)cap_buffers[i];
        cap_bd_list[i].length = CAP_BUFFER_SIZE / 2;  /* in dwords */
        cap_bd_list[i].ctrl   = 0;
    }
    cap_nabm_write32(NABM_PCM_IN_BD_LIST, (uint32_t)(uintptr_t)cap_bd_list);

    /* Configure the NAM mixer for mic input. */
    /* 0x1C = Record Select. 0x0000 = mic in (left+right). */
    cap_nam_write16(0x1C, 0x0000);
    /* 0x1A = Record Gain. 0x0F0F = max gain on both channels. */
    cap_nam_write16(0x1A, 0x0F0F);
    /* 0x0A = Mic Volume. 0x0008 = unmuted, ~12 dB. */
    cap_nam_write16(0x0A, 0x0008);

    /* Make sure the AC97 codec is set to 44100 Hz PCM. */
    /* 0x2A = PCM front DAC rate. */
    cap_nam_write16(0x2A, 44100);
    /* Some AC97 variants have a separate input sample rate register. */
    cap_nam_write16(0x10, 44100);  /* Mic ADC rate, if supported */

    /* Reset the ring buffer. */
    cap_ring_head = 0;
    cap_ring_tail = 0;

    cap_inited = 1;
    pr_info("ac97-cap: initialized, 44100 Hz 16-bit stereo, %u buffers, %u byte ring\n",
            (unsigned)NUM_CAP_BUFFERS, (unsigned)CAP_RING_BYTES);
    return 1;
}

/* Public: is capture available? */
int ac97_capture_available(void) {
    return cap_inited;
}

/* Internal: drain one hardware buffer (cap_buffers[idx]) into the ring. */
static void drain_buffer(int idx, uint32_t bytes) {
    if (bytes > CAP_BUFFER_SIZE) bytes = CAP_BUFFER_SIZE;
    const uint8_t* src = cap_buffers[idx];
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t next_head = (cap_ring_head + 1) % CAP_RING_BYTES;
        if (next_head == cap_ring_tail) {
            /* Ring full — drop the sample. STT engine is too slow. */
            break;
        }
        cap_ring[cap_ring_head] = src[i];
        cap_ring_head = next_head;
    }
}

/* Public: poll the hardware for fresh captured audio and push it into
 * the ring buffer. Called from the STT engine's poll loop (or from
 * the timer IRQ if you want true async capture). */
int ac97_capture_poll(void) {
    if (!cap_inited) return 0;

    int bytes_captured = 0;
    /* Check SR for completion of the current buffer. */
    uint8_t sr = cap_nabm_read8(NABM_PCM_IN_SR);
    if (sr & (SR_BCIS | SR_LVBCI)) {
        /* One or more buffers completed. Drain the current one and
         * advance. */
        uint8_t civ = cap_nabm_read8(NABM_PCM_IN_CIV);
        drain_buffer(civ, CAP_BUFFER_SIZE);
        bytes_captured += CAP_BUFFER_SIZE;

        /* Clear the status bits. */
        cap_nabm_write8(NABM_PCM_IN_SR, sr & (SR_BCIS | SR_LVBCI | SR_FIFOE));

        /* If we hit LVI, restart the bus master to keep capturing. */
        uint8_t lvi = cap_nabm_read8(NABM_PCM_IN_LVI);
        if (civ == lvi) {
            cap_nabm_write8(NABM_PCM_IN_CR, 0);   /* stop */
            cap_nabm_write8(NABM_PCM_IN_LVI, NUM_CAP_BUFFERS - 1);
            cap_nabm_write8(NABM_PCM_IN_CR, CR_RPBM | CR_IOCE | CR_FEIE);
        }
    }
    return bytes_captured;
}

/* Public: start continuous capture. */
int ac97_capture_start(void) {
    if (!cap_inited) return 0;
    cap_nabm_write8(NABM_PCM_IN_CR, 0);   /* stop if running */
    cap_nabm_write8(NABM_PCM_IN_LVI, NUM_CAP_BUFFERS - 1);
    /* Run + interrupt-on-completion + fifo-error-interrupt. */
    cap_nabm_write8(NABM_PCM_IN_CR, CR_RPBM | CR_IOCE | CR_FEIE);
    pr_info("ac97-cap: capture started\n");
    return 1;
}

/* Public: stop capture. */
int ac97_capture_stop(void) {
    if (!cap_inited) return 0;
    cap_nabm_write8(NABM_PCM_IN_CR, 0);
    pr_info("ac97-cap: capture stopped\n");
    return 1;
}

/* Public: drain up to `out_max` bytes from the ring into `out`.
 * Returns bytes actually copied (0 if ring empty). */
int ac97_capture_read(uint8_t* out, int out_max) {
    if (!cap_inited || !out || out_max <= 0) return 0;
    int copied = 0;
    while (copied < out_max && cap_ring_tail != cap_ring_head) {
        out[copied++] = cap_ring[cap_ring_tail];
        cap_ring_tail = (cap_ring_tail + 1) % CAP_RING_BYTES;
    }
    return copied;
}

/* Public: how many bytes are available in the ring right now? */
int ac97_capture_available_bytes(void) {
    int32_t delta = (int32_t)(cap_ring_head - cap_ring_tail);
    if (delta < 0) delta += CAP_RING_BYTES;
    return delta;
}

/* Public: compute RMS amplitude of the last N samples (0..32767).
 * Useful for the STT waveform animation in the top bar. */
int ac97_capture_rms(int window_samples) {
    if (!cap_inited) return 0;
    if (window_samples <= 0 || window_samples > 1024) window_samples = 256;

    /* Each stereo sample = 4 bytes (2 × 16-bit). Walk the ring backwards. */
    int bytes_needed = window_samples * 4;
    int avail = ac97_capture_available_bytes();
    if (avail < bytes_needed) bytes_needed = avail;

    uint32_t sq_sum = 0;
    int count = 0;
    int pos = (int)cap_ring_head;
    for (int i = 0; i < bytes_needed; i += 4) {
        pos -= 4;
        if (pos < 0) pos += CAP_RING_BYTES;
        int16_t left = (int16_t)((uint16_t)cap_ring[pos] |
                                  ((uint16_t)cap_ring[pos + 1] << 8));
        sq_sum += (uint32_t)(left * left);
        count++;
    }
    if (count == 0) return 0;
    /* sqrt(sq_sum / count) — but sqrt is expensive; use isqrt. */
    uint32_t mean = sq_sum / count;
    /* Integer sqrt. */
    uint32_t root = 0;
    uint32_t bit = 0x8000;
    while (bit) {
        uint32_t trial = root + bit;
        if (trial * trial <= mean) root = trial;
        bit >>= 1;
    }
    return (int)root;
}
