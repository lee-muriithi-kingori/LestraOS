/*
 * Lestra OS - CMOS Real-Time Clock Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Reads the current time and date from the MC146818-compatible CMOS
 * RTC via I/O ports 0x70 (index) / 0x71 (data). Supports both BCD and
 * binary modes, and 12/24-hour formats. Provides a Unix-epoch
 * timestamp for the rest of the kernel (cron, logging, etc.).
 *
 * CMOS register map (offset in 0x70):
 *   0x00 = seconds, 0x02 = minutes, 0x04 = hours,
 *   0x06 = weekday,  0x07 = day,     0x08 = month,
 *   0x09 = year (2-digit), 0x32 = century (2-digit)
 *   0x0A = status A (bit 7 = update in progress)
 *   0x0B = status B (bit 1 = 24h, bit 2 = binary mode)
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/rtc.h>

/* CMOS I/O ports. */
#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

/* CMOS register indices. */
#define CMOS_SEC       0x00
#define CMOS_MIN       0x02
#define CMOS_HOUR      0x04
#define CMOS_WDAY      0x06
#define CMOS_DAY       0x07
#define CMOS_MONTH     0x08
#define CMOS_YEAR      0x09
#define CMOS_CENTURY   0x32
#define CMOS_STATUS_A  0x0A
#define CMOS_STATUS_B  0x0B

/* Status B bits. */
#define STB_24HOUR   0x02   /* Set = 24-hour mode, clear = 12-hour   */
#define STB_BINARY   0x04   /* Set = binary, clear = BCD             */

/* Status A bits. */
#define STA_UPDATING 0x80   /* Set = RTC update in progress          */

static int initialized = 0;
static int use_binary  = 0;   /* 1 = CMOS already in binary mode     */
static int use_24hour  = 1;   /* 1 = 24-hour format                  */

static inline uint8_t cmos_read(uint8_t reg) {
    /* Bit 7 of the index write controls NMI (1 = masked). We keep NMI
     * enabled by masking the register index with 0x7F. */
    outb(CMOS_ADDR, reg & 0x7F);
    io_wait();
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_binary(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

/* Wait until the RTC is not mid-update so we don't read torn values.
 * Returns 1 if the RTC became idle, 0 if it never settled. */
static int wait_ready(void) {
    int tries = 65536;
    while (tries-- > 0) {
        if ((cmos_read(CMOS_STATUS_A) & STA_UPDATING) == 0) {
            return 1;
        }
    }
    return 0;
}

void rtc_init(void) {
    pr_info("rtc: initialising CMOS RTC (ports 0x70/0x71)\n");

    uint8_t stb = cmos_read(CMOS_STATUS_B);
    use_binary = (stb & STB_BINARY) ? 1 : 0;
    use_24hour = (stb & STB_24HOUR) ? 1 : 0;

    pr_info("rtc: mode = %s / %s\n",
            use_binary ? "binary" : "BCD",
            use_24hour ? "24-hour" : "12-hour");

    /* Sanity probe: the seconds register should never read 0xFF. */
    uint8_t s = cmos_read(CMOS_SEC);
    if (s == 0xFF) {
        pr_warn("rtc: CMOS not responding, time will be bogus\n");
    } else {
        uint16_t y; uint8_t mo, d, h, mi, se;
        rtc_get_date(&y, &mo, &d);
        rtc_get_time(&h, &mi, &se);
        pr_info("rtc: current time %u-%u-%u %u:%u:%u\n",
                (unsigned)y, (unsigned)mo, (unsigned)d,
                (unsigned)h, (unsigned)mi, (unsigned)se);
    }

    pr_info("rtc: unix time = %u\n", (unsigned)rtc_get_unix_time());

    initialized = 1;
}

/* Decode a raw CMOS byte: BCD -> binary if needed. */
static uint8_t decode(uint8_t raw) {
    if (!use_binary) {
        return bcd_to_binary(raw);
    }
    return raw;
}

void rtc_get_time(uint8_t* hour, uint8_t* min, uint8_t* sec) {
    uint8_t h = 0, m = 0, s = 0;

    if (initialized) {
        /* Read twice and retry until stable — the classic RTC update
         * race where the register changes between reads. */
        uint8_t h1 = 0, m1 = 0, s1 = 0;
        uint8_t h2 = 0xFF, m2 = 0xFF, s2 = 0xFF;
        int tries = 4;
        do {
            if (!wait_ready()) break;
            s1 = cmos_read(CMOS_SEC);
            m1 = cmos_read(CMOS_MIN);
            h1 = cmos_read(CMOS_HOUR);
            if (!wait_ready()) break;
            s2 = cmos_read(CMOS_SEC);
            m2 = cmos_read(CMOS_MIN);
            h2 = cmos_read(CMOS_HOUR);
        } while (tries-- > 0 && (s1 != s2 || m1 != m2 || h1 != h2));

        /* In 12-hour mode bit 7 of the hours register is the PM flag;
         * mask it off before BCD conversion. */
        uint8_t pm = 0;
        if (!use_24hour && (h1 & 0x80)) {
            pm = 1;
            h1 = (uint8_t)(h1 & 0x7F);
        }

        s = decode(s1);
        m = decode(m1);
        h = decode(h1);

        /* Convert 12-hour to 24-hour: 12 AM = 0, 1-11 AM = 1-11,
         * 12 PM = 12, 1-11 PM = 13-23. */
        if (!use_24hour) {
            if (pm && h < 12) {
                h = (uint8_t)(h + 12);
            } else if (!pm && h == 12) {
                h = 0;
            }
        }
    }

    if (hour) *hour = h;
    if (min)  *min  = m;
    if (sec)  *sec  = s;
}

void rtc_get_date(uint16_t* year, uint8_t* month, uint8_t* day) {
    uint16_t y = 1970;
    uint8_t  mo = 1, d = 1;

    if (initialized) {
        uint8_t d1 = 0, mo1 = 0, y1 = 0, c1 = 0;
        uint8_t d2 = 0xFF, mo2 = 0xFF, y2 = 0xFF, c2 = 0xFF;
        int tries = 4;
        do {
            if (!wait_ready()) break;
            d1  = cmos_read(CMOS_DAY);
            mo1 = cmos_read(CMOS_MONTH);
            y1  = cmos_read(CMOS_YEAR);
            c1  = cmos_read(CMOS_CENTURY);
            if (!wait_ready()) break;
            d2  = cmos_read(CMOS_DAY);
            mo2 = cmos_read(CMOS_MONTH);
            y2  = cmos_read(CMOS_YEAR);
            c2  = cmos_read(CMOS_CENTURY);
        } while (tries-- > 0 && (d1 != d2 || mo1 != mo2 || y1 != y2 || c1 != c2));

        d1  = decode(d1);
        mo1 = decode(mo1);
        y1  = decode(y1);
        c1  = decode(c1);

        /* Some firmwares don't populate the century register. Fall
         * back to 2000 + 2-digit year (good enough for most RTCs). */
        if (c1 == 0 || c1 == 0xFF) {
            y = (uint16_t)(y1 < 70 ? 2000 : 1900) + y1;
        } else {
            y = (uint16_t)c1 * 100 + y1;
        }

        mo = mo1;
        d  = d1;
    }

    if (year)  *year  = y;
    if (month) *month = mo;
    if (day)   *day   = d;
}

static int is_leap_year(int year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int days_in_month(int month, int year) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && is_leap_year(year)) return 29;
    return dim[month - 1];
}

uint64_t rtc_get_unix_time(void) {
    uint16_t year;  uint8_t month, day;
    uint8_t hour = 0, min = 0, sec = 0;

    rtc_get_date(&year, &month, &day);
    rtc_get_time(&hour, &min, &sec);

    if (year < 1970) {
        year = 1970;
    }

    /* Days from 1970-01-01 to start of the current year. */
    uint64_t days = 0;
    for (int y = 1970; y < (int)year; y++) {
        days += (uint64_t)(is_leap_year(y) ? 366 : 365);
    }
    /* Days from Jan 1 to start of the current month. */
    for (int m = 1; m < (int)month; m++) {
        days += (uint64_t)days_in_month(m, (int)year);
    }
    /* Days elapsed this month (day-of-month is 1-based). */
    if (day >= 1) {
        days += (uint64_t)(day - 1);
    }

    return days * 86400ULL
         + (uint64_t)hour * 3600ULL
         + (uint64_t)min  * 60ULL
         + (uint64_t)sec;
}
