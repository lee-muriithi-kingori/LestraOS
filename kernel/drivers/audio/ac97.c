/*
 * Lestra OS - AC97 Audio Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Intel AC97 audio controller (PCI class 04:01:00). Supports PCM
 * playback (mono/stereo, 16-bit, 44100 Hz).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/pci.h>
#include <string.h>

#define NABM_PCM_OUT_BD_LIST  0x10
#define NABM_PCM_OUT_CIV      0x14
#define NABM_PCM_OUT_LVI      0x15
#define NABM_PCM_OUT_SR       0x16
#define NABM_PCM_OUT_CR       0x1B
#define NABM_GLOBAL_CTRL      0x2C
#define NABM_GLOBAL_STATUS    0x30

#define CR_RPA                0x01
#define CR_RPBM               0x02

struct bd_entry {
    uint32_t addr;
    uint16_t length;
    uint16_t ctrl;
} __packed;

#define NUM_BUFFERS 8
#define BUFFER_SIZE 4096

static uintptr_t nabm_base = 0;
static uintptr_t nam_base = 0;
static int ac97_present = 0;

static struct bd_entry bd_list[NUM_BUFFERS] __aligned(16);
static uint8_t audio_buffers[NUM_BUFFERS][BUFFER_SIZE] __aligned(128);

static inline uint8_t nabm_read8(uint32_t off) { return inb(nabm_base + off); }
static inline uint16_t nabm_read16(uint32_t off) { return inw(nabm_base + off); }
static inline uint32_t nabm_read32(uint32_t off) { return inl(nabm_base + off); }
static inline void nabm_write8(uint32_t off, uint8_t val) { outb(nabm_base + off, val); }
static inline void nabm_write16(uint32_t off, uint16_t val) { outw(nabm_base + off, val); }
static inline void nabm_write32(uint32_t off, uint32_t val) { outl(nabm_base + off, val); }

static inline uint16_t nam_read16(uint8_t offset) {
    outw(nam_base + 0x04, offset);
    return inw(nam_base + 0x06);
}
static inline void nam_write16(uint8_t offset, uint16_t val) {
    outw(nam_base + 0x04, offset);
    outw(nam_base + 0x06, val);
}

static int ac97_find(uint8_t* bus, uint8_t* dev, uint8_t* func) {
    for (uint8_t d = 0; d < 32; d++) {
        for (uint8_t f = 0; f < 8; f++) {
            uint32_t id = pci_config_read32(0, d, f, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint32_t class_code = pci_config_read32(0, d, f, 0x08);
            uint8_t base_class = (class_code >> 24) & 0xFF;
            uint8_t subclass = (class_code >> 16) & 0xFF;
            if (base_class == 0x04 && subclass == 0x01) {
                *bus = 0; *dev = d; *func = f;
                return 1;
            }
        }
    }
    return 0;
}

int ac97_init(void);
int ac97_is_present(void);
int ac97_play(const void* buf, uint32_t len);
void ac97_set_master_volume(int volume /* 0..100 */);

int ac97_init(void) {
    uint8_t bus, dev, func;
    if (!ac97_find(&bus, &dev, &func)) {
        pr_info("ac97: no AC97 controller found\n");
        return 0;
    }

    uint32_t bar0 = pci_config_read32(bus, dev, func, 0x10);
    uint32_t bar1 = pci_config_read32(bus, dev, func, 0x14);
    nabm_base = bar0 & ~0xFu;
    nam_base = bar1 & ~0xFu;

    uint32_t cmd = pci_config_read32(bus, dev, func, 0x04);
    pci_config_write32(bus, dev, func, 0x04, cmd | 0x5);

    pr_info("ac97: found at PCI 00:%u.%u, NABM=0x%x, NAM=0x%x\n",
            (unsigned)dev, (unsigned)func,
            (unsigned)nabm_base, (unsigned)nam_base);

    nabm_write8(NABM_PCM_OUT_CR, CR_RPA);
    int timeout = 10000;
    while ((nabm_read8(NABM_PCM_OUT_CR) & CR_RPA) && timeout-- > 0);

    nabm_write16(NABM_GLOBAL_CTRL, 0x0002);
    timeout = 10000;
    while (!(nabm_read16(NABM_GLOBAL_STATUS) & 0x100) && timeout-- > 0);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        bd_list[i].addr = (uint32_t)(uintptr_t)audio_buffers[i];
        bd_list[i].length = BUFFER_SIZE / 2;
        bd_list[i].ctrl = 0;
    }
    nabm_write32(NABM_PCM_OUT_BD_LIST, (uint32_t)(uintptr_t)bd_list);

    nam_write16(0x02, 0x0000);
    nam_write16(0x18, 0x0000);
    nam_write16(0x2A, 44100);

    ac97_present = 1;
    pr_info("ac97: initialized, 44100 Hz, 16-bit stereo, %u buffers\n",
            (unsigned)NUM_BUFFERS);
    return 1;
}

int ac97_is_present(void) { return ac97_present; }

/* AC97 Master Volume register (NAM offset 0x02).
 *
 * Register layout (16-bit, stereo):
 *   bits [15:8]  = right channel attenuation
 *   bits [7:0]   = left channel attenuation
 * Each channel field is 6 bits wide (bits [5:0] of each byte):
 *   0x00 = 0 dB (maximum)
 *   0x3F = -94.5 dB (effectively mute)
 *   1.5 dB per step
 *
 * Caller passes a 0..100 user volume where 0 = mute and 100 = max.
 * Conversion: ac97_att = (100 - vol) * 0x3F / 100
 *   vol=100 -> att=0x00 (max), vol=0 -> att=0x3F (mute)
 *
 * Access is via PIO (index port nam_base+0x04, data port nam_base+0x06),
 * NOT MMIO — the NAM BAR is an I/O port pair, established in ac97_init().
 * Silently no-ops if the AC97 controller was not initialised (matches
 * the previous weak-stub behaviour so callers in the GUI layer don't
 * need to check ac97_is_present() themselves).
 */
void ac97_set_master_volume(int volume) {
    if (!ac97_present) return;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    uint16_t att = (uint16_t)(((100 - volume) * 0x3F) / 100);
    /* Same attenuation on both channels (stereo linked). */
    uint16_t reg = (uint16_t)((att << 8) | att);
    nam_write16(0x02, reg);
}

int ac97_play(const void* buf, uint32_t len) {
    if (!ac97_present || !buf || len == 0) return 0;

    uint32_t remaining = len;
    const uint8_t* src = (const uint8_t*)buf;
    int buffers_filled = 0;

    for (int i = 0; i < NUM_BUFFERS && remaining > 0; i++) {
        uint32_t to_copy = BUFFER_SIZE;
        if (to_copy > remaining) to_copy = remaining;
        memcpy(audio_buffers[i], src, to_copy);
        if (to_copy < BUFFER_SIZE) {
            memset(audio_buffers[i] + to_copy, 0, BUFFER_SIZE - to_copy);
        }
        bd_list[i].length = BUFFER_SIZE / 2;
        src += to_copy;
        remaining -= to_copy;
        buffers_filled++;
    }

    if (buffers_filled == 0) return 0;

    nabm_write8(NABM_PCM_OUT_LVI, buffers_filled - 1);
    nabm_write8(NABM_PCM_OUT_CR, CR_RPBM);

    int timeout = 10000000;
    while (timeout-- > 0) {
        uint8_t sr = nabm_read8(NABM_PCM_OUT_SR);
        if (sr & 0x08) break;
    }

    nabm_write8(NABM_PCM_OUT_CR, 0);
    return (int)(len - remaining);
}
