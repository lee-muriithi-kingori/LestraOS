/*
 * Lestra OS - Media Player card (with WAV/PCM codec + tone generator)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Implements:
 *   - WAV/PCM codec: parses RIFF header, "fmt " chunk, "data" chunk
 *   - Supports 8-bit and 16-bit PCM, mono and stereo, 8000-48000 Hz
 *   - Plays decoded PCM through the AC97 audio driver
 *   - Tone generator fallback: sine wave at configurable frequency/duration
 *   - Media player UI: track name, metadata display, play/pause/stop
 *
 * Limitations:
 *   - Video codecs are not implemented (H.264/MPEG/VP8 etc. are huge)
 *   - AC97 is hardcoded to 44100 Hz stereo 16-bit; we resample/rescale
 *     on-the-fly for non-44100/mono/8-bit WAV files
 *   - No streaming; entire WAV data chunk is loaded into memory
 *   - No volume control beyond mute/unmute
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/vfs.h>
#include <string.h>

#define MEDIA_W  480
#define MEDIA_H  360
#define MEDIA_TITLE_H 36

/* ----- WAV header structures (little-endian, packed) ----- */

struct wav_riff_header {
    uint8_t  riff_id[4];     /* "RIFF" */
    uint32_t file_size;      /* file size - 8 */
    uint8_t  wave_id[4];     /* "WAVE" */
} __packed;

struct wav_fmt_chunk {
    uint8_t  fmt_id[4];     /* "fmt " */
    uint32_t chunk_size;    /* size of fmt chunk data */
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;  /* 1 = mono, 2 = stereo */
    uint32_t sample_rate;   /* e.g. 44100 */
    uint32_t byte_rate;     /* sample_rate * block_align */
    uint16_t block_align;   /* num_channels * bits_per_sample / 8 */
    uint16_t bits_per_sample; /* 8 or 16 */
} __packed;

struct wav_data_chunk_hdr {
    uint8_t  data_id[4];    /* "data" */
    uint32_t chunk_size;    /* size of PCM data */
} __packed;

/* ----- WAV/PCM codec state ----- */

enum media_mode {
    MEDIA_MODE_NONE,       /* no media loaded */
    MEDIA_MODE_WAV,        /* WAV file loaded */
    MEDIA_MODE_TONE,       /* tone generator active */
};

struct media_state {
    int active;
    int playing;
    int progress;          /* 0..1000 */
    enum media_mode mode;

    /* Track metadata */
    char track_name[64];
    uint32_t wav_sample_rate;
    uint16_t wav_channels;
    uint16_t wav_bits;
    uint32_t wav_duration_ms;   /* calculated duration */

    /* WAV data buffer (pointer into static PCM buffer) */
    uint8_t* wav_data;
    uint32_t wav_data_size;

    /* Tone generator parameters */
    uint32_t tone_freq;         /* Hz (default 440) */
    uint32_t tone_duration_ms;  /* ms (default 1000) */

    /* Playback state */
    uint32_t play_offset;       /* current byte offset in data */
    uint64_t play_start_ms;     /* when playback started */
};

static struct media_state media_state;
static struct widget media_widget;

/* ----- PCM output buffer for AC97 -----
 * AC97 expects 16-bit stereo @ 44100 Hz. We convert/resample into this
 * buffer before calling ac97_play(). Max ~5 seconds of audio at once. */
#define PCM_BUF_MAX_SAMPLES (44100 * 2 * 5)   /* 5 sec stereo 16-bit */
static int16_t pcm_output_buf[PCM_BUF_MAX_SAMPLES];

/* ----- WAV file data buffer -----
 * We load the entire data chunk from VFS. Max 256 KB of raw WAV data. */
#define WAV_DATA_BUF_SIZE (256 * 1024)
static uint8_t wav_data_buf[WAV_DATA_BUF_SIZE];

/* ----- sin lookup table (for tone generator) -----
 * 256-entry, one full cycle, returns -1000..1000. */
static const int16_t sin_table[256] = {
    0,25,50,74,98,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,460,482,504,525,546,566,586,605,624,642,660,676,692,
    707,721,734,746,757,766,775,782,788,793,797,799,800,800,799,796,
    792,787,780,772,762,751,739,726,711,695,678,660,640,620,598,575,
    551,526,500,473,445,417,388,358,327,296,264,232,200,167,134,101,
    67,34,0,-34,-67,-101,-134,-167,-200,-232,-264,-296,-327,-358,-388,-417,
    -445,-473,-500,-526,-551,-575,-598,-620,-640,-660,-678,-695,-711,-726,-739,-751,
    -762,-772,-780,-787,-792,-796,-799,-800,-800,-800,-797,-793,-788,-782,-775,-766,
    -757,-746,-734,-721,-707,-692,-676,-660,-642,-624,-605,-586,-566,-546,-525,-504,
    -482,-460,-438,-415,-392,-369,-345,-321,-297,-273,-249,-224,-200,-175,-150,-125,
    -98,-74,-50,-25,0,25,50,74,98,125,150,175,200,224,249,273,
    297,321,345,369,392,415,438,460,482,504,525,546,566,586,605,624,
    642,660,676,692,707,721,734,746,757,766,775,782,788,793,797,799,
    800,800,799,796,792,787,780,772,762,751,739,726,711,695,678,660,
    640,620,598,575,551,526,500,473,445,417,388,358,327,296,264,232,
    200,167,134,101,67,34,0
};

/* isin(phase) where phase is 0..1000 mapping to 0..2*pi. Returns -1000..1000. */
static int media_isin(uint32_t phase_milli) {
    uint32_t idx = (phase_milli * 256) / 1000;
    return sin_table[idx & 0xFF];
}

/* ----- WAV parser -----
 * Reads a WAV file from VFS, parses RIFF/fmt/data headers, validates
 * the format, and sets up the media_state for playback. Returns 0 on
 * success, -1 on error. */
static int wav_parse(const char* path) {
    extern int vfs_open(const char* path, int flags);
    extern ssize_t vfs_read(int fd, void* buf, size_t count);
    extern int vfs_close(int fd);

    int fd = vfs_open(path, 0);   /* O_RDONLY = 0 */
    if (fd < 0) return -1;

    /* Read the entire file into wav_data_buf (up to WAV_DATA_BUF_SIZE) */
    ssize_t total = vfs_read(fd, wav_data_buf, WAV_DATA_BUF_SIZE);
    vfs_close(fd);
    if (total < (ssize_t)sizeof(struct wav_riff_header)) return -1;

    /* Parse RIFF header */
    struct wav_riff_header* riff = (struct wav_riff_header*)wav_data_buf;
    if (riff->riff_id[0] != 'R' || riff->riff_id[1] != 'I' ||
        riff->riff_id[2] != 'F' || riff->riff_id[3] != 'F') return -1;
    if (riff->wave_id[0] != 'W' || riff->wave_id[1] != 'A' ||
        riff->wave_id[2] != 'V' || riff->wave_id[3] != 'E') return -1;

    /* Walk chunks to find "fmt " and "data" */
    struct wav_fmt_chunk* fmt = NULL;
    struct wav_data_chunk_hdr* data_hdr = NULL;
    uint8_t* p = wav_data_buf + sizeof(struct wav_riff_header);
    uint8_t* end = wav_data_buf + total;

    while (p + 8 < end) {
        /* Read chunk ID and size */
        uint8_t id[4];
        memcpy(id, p, 4);
        uint32_t chunk_sz;
        memcpy(&chunk_sz, p + 4, 4);   /* little-endian */

        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ') {
            fmt = (struct wav_fmt_chunk*)p;
            /* Validate: PCM format only */
            if (fmt->audio_format != 1) {
                /* Not PCM — unsupported compressed format */
                return -1;
            }
            /* Validate: supported sample rates */
            if (fmt->sample_rate < 8000 || fmt->sample_rate > 48000) return -1;
            /* Validate: 8 or 16 bit */
            if (fmt->bits_per_sample != 8 && fmt->bits_per_sample != 16) return -1;
            /* Validate: mono or stereo */
            if (fmt->num_channels != 1 && fmt->num_channels != 2) return -1;
        }

        if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            data_hdr = (struct wav_data_chunk_hdr*)p;
        }

        /* Advance to next chunk (chunk data follows the 8-byte header) */
        uint32_t advance = 8 + chunk_sz;
        /* WAV chunks are padded to even size */
        if (chunk_sz & 1) advance++;
        p += advance;
        if (p > end) break;
    }

    if (!fmt || !data_hdr) return -1;

    /* Locate PCM data */
    uint8_t* data_start = (uint8_t*)data_hdr + sizeof(struct wav_data_chunk_hdr);
    uint32_t data_size = data_hdr->chunk_size;

    /* Bounds check */
    if (data_start + data_size > end) {
        data_size = (uint32_t)(end - data_start);
    }

    /* Calculate duration in milliseconds */
    uint32_t block_align = fmt->block_align;
    if (block_align == 0) block_align = fmt->num_channels * fmt->bits_per_sample / 8;
    uint32_t total_samples = data_size / block_align;
    uint32_t duration_ms = (total_samples * 1000) / fmt->sample_rate;

    /* Store in media state */
    media_state.mode = MEDIA_MODE_WAV;
    media_state.wav_sample_rate = fmt->sample_rate;
    media_state.wav_channels = fmt->num_channels;
    media_state.wav_bits = fmt->bits_per_sample;
    media_state.wav_duration_ms = duration_ms;
    media_state.wav_data = data_start;
    media_state.wav_data_size = data_size;
    media_state.playing = 0;
    media_state.progress = 0;
    media_state.play_offset = 0;

    /* Format track name from path */
    const char* basename = path;
    for (const char* s = path; *s; s++) {
        if (*s == '/') basename = s + 1;
    }
    strncpy(media_state.track_name, basename, sizeof(media_state.track_name) - 1);
    media_state.track_name[sizeof(media_state.track_name) - 1] = '\0';

    return 0;
}

/* ----- PCM conversion -----
 * Converts a chunk of WAV data to AC97 format (16-bit stereo 44100 Hz).
 * Handles:
 *   - 8-bit unsigned -> 16-bit signed
 *   - Mono -> stereo duplication
 *   - Sample rate conversion (simple nearest-neighbor resampling)
 *
 * Returns number of 16-bit stereo samples written to pcm_output_buf.
 * Max output is PCM_BUF_MAX_SAMPLES. */
static int wav_to_ac44(uint32_t offset, int max_output_samples) {
    if (media_state.mode != MEDIA_MODE_WAV) return 0;
    if (!media_state.wav_data) return 0;

    uint8_t* src = media_state.wav_data + offset;
    uint32_t remaining = media_state.wav_data_size - offset;
    if (remaining == 0 || offset >= media_state.wav_data_size) return 0;

    uint16_t channels = media_state.wav_channels;
    uint16_t bits = media_state.wav_bits;
    uint32_t src_rate = media_state.wav_sample_rate;
    uint32_t dst_rate = 44100;
    uint16_t block_align = channels * (bits / 8);

    int out_idx = 0;
    uint32_t src_frame = 0;    /* which source frame we're reading */

    /* Resampling ratio: how many source frames per output frame */
    /* We'll use nearest-neighbor: output frame i maps to source frame
     * (i * src_rate) / dst_rate */
    while (out_idx < max_output_samples) {
        /* Which source frame? */
        uint32_t src_frame_idx = ((uint32_t)out_idx * src_rate) / dst_rate;
        uint32_t src_byte_off = src_frame_idx * block_align;

        if (src_byte_off >= remaining) break;

        int16_t left, right;

        if (bits == 16) {
            /* 16-bit signed PCM */
            int16_t* s16 = (int16_t*)(src + src_byte_off);
            if (channels == 1) {
                left = right = s16[0];
            } else {
                left = s16[0];
                right = s16[1];
            }
        } else {
            /* 8-bit unsigned PCM: convert to signed 16-bit */
            uint8_t* s8 = src + src_byte_off;
            if (channels == 1) {
                int16_t val = (int16_t)((s8[0] - 128) * 256);
                left = right = val;
            } else {
                left = (int16_t)((s8[0] - 128) * 256);
                right = (int16_t)((s8[1] - 128) * 256);
            }
        }

        pcm_output_buf[out_idx * 2] = left;
        pcm_output_buf[out_idx * 2 + 1] = right;
        out_idx++;
    }

    return out_idx;   /* number of stereo frames (each = 2 × int16_t) */
}

/* ----- Tone generator -----
 * Generates a sine wave PCM at the given frequency and duration,
 * directly in AC97 format (16-bit stereo 44100 Hz). Returns number
 * of stereo frames produced. */
static int tone_generate(uint32_t freq, uint32_t duration_ms,
                         int max_output_samples) {
    uint32_t total_samples_441 = (44100 * duration_ms) / 1000;
    if (total_samples_441 > (uint32_t)max_output_samples)
        total_samples_441 = max_output_samples;

    for (uint32_t i = 0; i < total_samples_441; i++) {
        /* phase: i * freq / 44100 gives fraction of full cycle.
         * Map to 0..1000 range for media_isin. */
        uint32_t phase_raw = (i * freq) / 44100;
        uint32_t phase_milli = (phase_raw % 1) * 1000;  /* fractional part */
        /* Actually: phase in cycles = (i * freq) / 44100
         * We want milli-cycles = (i * freq * 1000) / 44100, but that
         * overflows uint32 for long durations. Use modular approach: */
        uint64_t phase64 = ((uint64_t)i * (uint64_t)freq * 1000ULL) / 44100ULL;
        uint32_t pm = (uint32_t)(phase64 % 1000);
        int val = (media_isin(pm) * 16000) / 1000;   /* amplitude ~16000 */
        pcm_output_buf[i * 2] = (int16_t)val;
        pcm_output_buf[i * 2 + 1] = (int16_t)val;
    }

    return (int)total_samples_441;
}

/* ----- AC97 playback helpers ----- */

extern int ac97_is_present(void);
extern int ac97_play(const void* buf, uint32_t len);

static int media_audio_present(void) {
    return ac97_is_present();
}

/* Play a chunk of audio from the current offset. Updates progress.
 * Returns 1 if more data remains, 0 if playback is done. */
static int media_play_chunk(void) {
    struct media_state* st = &media_state;

    if (!media_audio_present()) return 0;

    int max_frames = PCM_BUF_MAX_SAMPLES / 2;  /* stereo frames */
    int frames = 0;

    if (st->mode == MEDIA_MODE_WAV) {
        frames = wav_to_ac44(st->play_offset, max_frames);
    } else if (st->mode == MEDIA_MODE_TONE) {
        frames = tone_generate(st->tone_freq, st->tone_duration_ms, max_frames);
        /* Tone plays once, no offset tracking */
        st->play_offset = st->wav_data_size;  /* mark as "done" */
    }

    if (frames <= 0) {
        /* Playback finished */
        st->playing = 0;
        st->progress = 1000;
        return 0;
    }

    /* Play through AC97 */
    uint32_t bytes = (uint32_t)frames * 2 * sizeof(int16_t);  /* stereo 16-bit */
    ac97_play(pcm_output_buf, bytes);

    /* Update offset for WAV mode */
    if (st->mode == MEDIA_MODE_WAV) {
        /* How many source bytes did we consume? */
        uint32_t src_rate = st->wav_sample_rate;
        uint32_t block_align = st->wav_channels * (st->wav_bits / 8);
        uint32_t src_frames_used = ((uint32_t)frames * src_rate) / 44100;
        st->play_offset += src_frames_used * block_align;

        /* Update progress */
        if (st->wav_data_size > 0) {
            st->progress = (int)((st->play_offset * 1000) / st->wav_data_size);
            if (st->progress > 1000) st->progress = 1000;
        }

        /* Check if we're done */
        if (st->play_offset >= st->wav_data_size) {
            st->playing = 0;
            st->progress = 1000;
            return 0;
        }
    }

    return 1;  /* more data remains */
}

/* ----- Drawing helpers ----- */

static void format_duration(char* buf, size_t buf_size, uint32_t ms) {
    uint32_t total_sec = ms / 1000;
    uint32_t min = total_sec / 60;
    uint32_t sec = total_sec % 60;
    ksnprintf(buf, buf_size, "%u:%02u", (unsigned)min, (unsigned)sec);
}

static void media_draw(struct widget* w);
static void media_on_event(struct widget* w, struct event* e);

static void media_draw(struct widget* w) {
    struct media_state* st = (struct media_state*)w->state;

    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, st->active ? UI_ACCENT : UI_CARD_BORDER);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, MEDIA_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Media Player", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    int body_x = w->x + 16;
    int body_y = w->y + MEDIA_TITLE_H + 16;

    /* Video preview area (black box) */
    int vid_w = w->w - 32;
    int vid_h = 100;
    fb_fill_rect(body_x, body_y, vid_w, vid_h, 0xFF000000);
    fb_draw_rect(body_x, body_y, vid_w, vid_h, UI_CARD_BORDER);

    /* Animated waveform visualization if playing */
    if (st->playing && (st->mode == MEDIA_MODE_WAV || st->mode == MEDIA_MODE_TONE)) {
        uint64_t now = timer_get_ms();
        uint32_t phase = (uint32_t)((now % 2000) * 1000 / 2000);
        for (int x = 0; x < vid_w; x++) {
            uint32_t p2 = (phase + x * 3) % 1000;
            int val = media_isin(p2);
            int bar_h = (val * 40) / 1000;
            if (bar_h < 0) bar_h = -bar_h;
            int cy = body_y + vid_h / 2;
            uint32_t color = (val > 0) ? 0x8022D3EE : 0x80F87171;
            if (bar_h > 0) {
                if (val > 0) {
                    fb_fill_rect(body_x + x, cy - bar_h, 1, bar_h, color);
                } else {
                    fb_fill_rect(body_x + x, cy, 1, bar_h, color);
                }
            }
            /* Center line */
            fb_set_pixel(body_x + x, cy, 0x40FFFFFF);
        }
    } else {
        /* Static "no signal" pattern */
        uint64_t now = timer_get_ms();
        for (int y = 0; y < vid_h; y += 2) {
            if (((y + (int)(now / 50)) % 8) < 4) {
                fb_fill_rect(body_x, body_y + y, vid_w, 1, 0xFF0A0A0A);
            }
        }

        /* Center text */
        const char* msg;
        if (st->mode == MEDIA_MODE_NONE)
            msg = "No media loaded";
        else if (st->mode == MEDIA_MODE_WAV)
            msg = st->playing ? "Playing WAV..." : "WAV ready";
        else if (st->mode == MEDIA_MODE_TONE)
            msg = st->playing ? "Playing tone..." : "Tone generator";
        else
            msg = "Unknown mode";

        int mw = fb_text_width(msg);
        fb_draw_string(body_x + (vid_w - mw) / 2, body_y + vid_h / 2 - 8,
                       msg, UI_TEXT_MUTED);
    }

    /* Track name and metadata */
    int meta_y = body_y + vid_h + 12;
    if (st->track_name[0]) {
        fb_draw_string(body_x, meta_y, st->track_name, UI_TEXT_PRIMARY);
    }

    /* WAV metadata line */
    if (st->mode == MEDIA_MODE_WAV) {
        char meta_buf[128];
        char dur_buf[16];
        format_duration(dur_buf, sizeof(dur_buf), st->wav_duration_ms);
        ksnprintf(meta_buf, sizeof(meta_buf),
                  "%u Hz | %u-bit | %s | %s",
                  (unsigned)st->wav_sample_rate,
                  (unsigned)st->wav_bits,
                  st->wav_channels == 1 ? "mono" : "stereo",
                  dur_buf);
        fb_draw_string(body_x, meta_y + 16, meta_buf, UI_TEXT_MUTED);
    } else if (st->mode == MEDIA_MODE_TONE) {
        char meta_buf[64];
        ksnprintf(meta_buf, sizeof(meta_buf),
                  "Tone: %u Hz, %u ms",
                  (unsigned)st->tone_freq,
                  (unsigned)st->tone_duration_ms);
        fb_draw_string(body_x, meta_y + 16, meta_buf, UI_TEXT_MUTED);
    }

    /* Duration / progress display */
    if (st->mode == MEDIA_MODE_WAV && st->wav_duration_ms > 0) {
        char cur_buf[16], total_buf[16];
        uint32_t elapsed_ms = 0;
        if (st->playing) {
            elapsed_ms = (uint32_t)(timer_get_ms() - st->play_start_ms);
            if (elapsed_ms > st->wav_duration_ms)
                elapsed_ms = st->wav_duration_ms;
        } else if (st->progress == 1000) {
            elapsed_ms = st->wav_duration_ms;
        }
        format_duration(cur_buf, sizeof(cur_buf), elapsed_ms);
        format_duration(total_buf, sizeof(total_buf), st->wav_duration_ms);
        char time_buf[32];
        ksnprintf(time_buf, sizeof(time_buf), "%s / %s", cur_buf, total_buf);
        int tw = fb_text_width(time_buf);
        fb_draw_string(body_x + vid_w - tw, meta_y + 16, time_buf, UI_TEXT_MUTED);
    }

    /* Progress bar */
    int bar_y = meta_y + 36;
    int bar_w = vid_w;
    fb_fill_rect(body_x, bar_y, bar_w, 6, UI_TEXT_FAINT);
    int fill_w = (bar_w * st->progress) / 1000;
    fb_fill_rect(body_x, bar_y, fill_w, 6, UI_ACCENT);
    /* Rounded ends */
    fb_draw_rounded(body_x, bar_y - 1, fill_w > 8 ? fill_w : 8, 8, 4,
                    0x4022D3EE, UI_ACCENT);

    /* Control buttons */
    int btn_y = bar_y + 20;
    int btn_size = 32;
    int btn_gap = 10;
    int total_btns = 4;  /* prev(|<), play/pause(>/||), stop([]), next(>|) */
    int total_w = btn_size * total_btns + btn_gap * (total_btns - 1);
    int btn_x_start = body_x + (vid_w - total_w) / 2;

    /* Prev button */
    fb_draw_rounded(btn_x_start, btn_y, btn_size, btn_size, 8,
                    0x80121828, UI_CARD_BORDER);
    fb_draw_string(btn_x_start + 8, btn_y + 8, "|<", UI_TEXT_PRIMARY);

    /* Play/Pause button */
    int play_x = btn_x_start + btn_size + btn_gap;
    fb_draw_rounded(play_x, btn_y, btn_size, btn_size, 8,
                    st->playing ? 0x8022D3EE : 0x8022D3EE, UI_CARD_BORDER);
    fb_draw_string(play_x + 8, btn_y + 8,
                   st->playing ? "||" : " >", UI_TEXT_PRIMARY);

    /* Stop button */
    int stop_x = play_x + btn_size + btn_gap;
    fb_draw_rounded(stop_x, btn_y, btn_size, btn_size, 8,
                    0x80F87171, UI_CARD_BORDER);
    fb_draw_string(stop_x + 10, btn_y + 8, "[]", UI_TEXT_PRIMARY);

    /* Next button */
    int next_x = stop_x + btn_size + btn_gap;
    fb_draw_rounded(next_x, btn_y, btn_size, btn_size, 8,
                    0x80121828, UI_CARD_BORDER);
    fb_draw_string(next_x + 8, btn_y + 8, ">|", UI_TEXT_PRIMARY);

    /* Tone generator button */
    int tone_btn_x = body_x;
    int tone_btn_y = btn_y + btn_size + 8;
    fb_draw_rounded(tone_btn_x, tone_btn_y, 72, 24, 6,
                    st->mode == MEDIA_MODE_TONE ? 0x8022D3EE : 0x80121828,
                    UI_CARD_BORDER);
    fb_draw_string(tone_btn_x + 6, tone_btn_y + 4, "Tone 440", UI_TEXT_PRIMARY);

    /* Frequency adjustment labels */
    fb_draw_string(tone_btn_x + 80, tone_btn_y + 4, "+100", UI_TEXT_MUTED);
    fb_draw_string(tone_btn_x + 120, tone_btn_y + 4, "-100", UI_TEXT_MUTED);

    /* Info text at bottom */
    int info_y = w->y + w->h - 28;
    if (media_audio_present()) {
        fb_draw_string(body_x, info_y,
                       "WAV/PCM codec active | AC97 audio driver | Alt+T: tone",
                       UI_TEXT_FAINT);
    } else {
        fb_draw_string(body_x, info_y,
                       "No AC97 audio controller found — playback disabled",
                       UI_TEXT_FAINT);
    }
}

static void media_on_event(struct widget* w, struct event* e) {
    struct media_state* st = (struct media_state*)w->state;

    if (e->type == EV_MOUSE_DOWN) {
        st->active = 1;

        int body_x = w->x + 16;
        int body_y = w->y + MEDIA_TITLE_H + 16;
        int vid_w = w->w - 32;
        int vid_h = 100;
        int meta_y = body_y + vid_h + 12;
        int bar_y = meta_y + 36;
        int btn_y = bar_y + 20;
        int btn_size = 32;
        int btn_gap = 10;
        int total_btns = 4;
        int total_w = btn_size * total_btns + btn_gap * (total_btns - 1);
        int btn_x_start = body_x + (vid_w - total_w) / 2;

        int play_x = btn_x_start + btn_size + btn_gap;
        int stop_x = play_x + btn_size + btn_gap;

        /* Play/Pause button */
        if (e->mouse.x >= play_x && e->mouse.x < play_x + btn_size &&
            e->mouse.y >= btn_y && e->mouse.y < btn_y + btn_size) {
            if (st->mode == MEDIA_MODE_NONE) {
                /* Try to load default WAV file from VFS */
                if (wav_parse("/home/music.wav") == 0) {
                    st->playing = 1;
                    st->play_offset = 0;
                    st->progress = 0;
                    st->play_start_ms = timer_get_ms();
                    media_play_chunk();
                } else if (wav_parse("/tmp/test.wav") == 0) {
                    st->playing = 1;
                    st->play_offset = 0;
                    st->progress = 0;
                    st->play_start_ms = timer_get_ms();
                    media_play_chunk();
                } else {
                    /* No WAV file — switch to tone generator */
                    st->mode = MEDIA_MODE_TONE;
                    st->tone_freq = 440;
                    st->tone_duration_ms = 2000;
                    strncpy(st->track_name, "Tone: 440 Hz",
                            sizeof(st->track_name) - 1);
                    st->wav_duration_ms = st->tone_duration_ms;
                    st->playing = 1;
                    st->progress = 0;
                    st->play_offset = 0;
                    st->play_start_ms = timer_get_ms();
                    media_play_chunk();
                }
            } else if (st->playing) {
                /* Pause */
                st->playing = 0;
            } else {
                /* Resume / start */
                if (st->progress >= 1000) {
                    /* Restart from beginning */
                    st->play_offset = 0;
                    st->progress = 0;
                }
                st->playing = 1;
                st->play_start_ms = timer_get_ms();
                media_play_chunk();
            }
        }

        /* Stop button */
        if (e->mouse.x >= stop_x && e->mouse.x < stop_x + btn_size &&
            e->mouse.y >= btn_y && e->mouse.y < btn_y + btn_size) {
            st->playing = 0;
            st->progress = 0;
            st->play_offset = 0;
        }

        /* Tone generator button */
        int tone_btn_x = body_x;
        int tone_btn_y = btn_y + btn_size + 8;
        if (e->mouse.x >= tone_btn_x && e->mouse.x < tone_btn_x + 72 &&
            e->mouse.y >= tone_btn_y && e->mouse.y < tone_btn_y + 24) {
            st->mode = MEDIA_MODE_TONE;
            st->tone_freq = 440;
            st->tone_duration_ms = 2000;
            strncpy(st->track_name, "Tone: 440 Hz",
                    sizeof(st->track_name) - 1);
            st->wav_duration_ms = st->tone_duration_ms;
            st->playing = 1;
            st->progress = 0;
            st->play_offset = 0;
            st->play_start_ms = timer_get_ms();
            media_play_chunk();
        }

        /* Tone frequency +100 */
        if (e->mouse.x >= tone_btn_x + 80 && e->mouse.x < tone_btn_x + 120 &&
            e->mouse.y >= tone_btn_y && e->mouse.y < tone_btn_y + 24) {
            if (st->mode == MEDIA_MODE_TONE) {
                st->tone_freq += 100;
                if (st->tone_freq > 20000) st->tone_freq = 20000;
                ksnprintf(st->track_name, sizeof(st->track_name),
                          "Tone: %u Hz", (unsigned)st->tone_freq);
            }
        }

        /* Tone frequency -100 */
        if (e->mouse.x >= tone_btn_x + 120 && e->mouse.x < tone_btn_x + 160 &&
            e->mouse.y >= tone_btn_y && e->mouse.y < tone_btn_y + 24) {
            if (st->mode == MEDIA_MODE_TONE) {
                st->tone_freq -= 100;
                if (st->tone_freq < 100) st->tone_freq = 100;
                ksnprintf(st->track_name, sizeof(st->track_name),
                          "Tone: %u Hz", (unsigned)st->tone_freq);
            }
        }
    }

    /* Keyboard shortcuts for media player */
    if (e->type == EV_KEY_DOWN) {
        /* Space = play/pause */
        if (e->key.ascii == ' ' && w->focused) {
            if (st->playing) {
                st->playing = 0;
            } else {
                if (st->mode == MEDIA_MODE_NONE) {
                    st->mode = MEDIA_MODE_TONE;
                    st->tone_freq = 440;
                    st->tone_duration_ms = 2000;
                    strncpy(st->track_name, "Tone: 440 Hz",
                            sizeof(st->track_name) - 1);
                    st->wav_duration_ms = st->tone_duration_ms;
                }
                if (st->progress >= 1000) {
                    st->play_offset = 0;
                    st->progress = 0;
                }
                st->playing = 1;
                st->play_start_ms = timer_get_ms();
                media_play_chunk();
            }
        }
        /* 's' = stop */
        if (e->key.ascii == 's' && w->focused && !(e->key.mods & MOD_CTRL)) {
            st->playing = 0;
            st->progress = 0;
            st->play_offset = 0;
        }
        /* 't' = toggle tone generator (Alt+T) */
        if (e->key.ascii == 't' && (e->key.mods & MOD_ALT)) {
            if (st->mode == MEDIA_MODE_TONE) {
                st->mode = MEDIA_MODE_NONE;
                st->track_name[0] = '\0';
                st->playing = 0;
            } else {
                st->mode = MEDIA_MODE_TONE;
                st->tone_freq = 440;
                st->tone_duration_ms = 2000;
                strncpy(st->track_name, "Tone: 440 Hz",
                        sizeof(st->track_name) - 1);
                st->wav_duration_ms = st->tone_duration_ms;
                st->playing = 1;
                st->progress = 0;
                st->play_offset = 0;
                st->play_start_ms = timer_get_ms();
                media_play_chunk();
            }
        }
    }
}

/* ----- Playback tick -----
 * Called from the compositor's main loop (or a timer) to continue
 * streaming audio chunks while playing. Returns 1 if still playing. */
int media_tick(void) {
    struct media_state* st = &media_state;
    if (!st->playing) return 0;

    /* For WAV mode: play next chunk */
    if (st->mode == MEDIA_MODE_WAV) {
        return media_play_chunk();
    }
    /* For tone mode: tone was already played in one shot */
    if (st->mode == MEDIA_MODE_TONE) {
        /* Check if tone duration has elapsed */
        uint64_t elapsed = timer_get_ms() - st->play_start_ms;
        if (elapsed >= st->tone_duration_ms) {
            st->playing = 0;
            st->progress = 1000;
            return 0;
        }
        /* Update progress */
        st->progress = (int)((elapsed * 1000) / st->tone_duration_ms);
        if (st->progress > 1000) st->progress = 1000;
        return 1;
    }

    return 0;
}

struct widget* media_create(int x, int y) {
    memset(&media_state, 0, sizeof(media_state));
    media_state.mode = MEDIA_MODE_NONE;
    media_state.tone_freq = 440;
    media_state.tone_duration_ms = 2000;

    media_widget.x = x;
    media_widget.y = y;
    media_widget.w = MEDIA_W;
    media_widget.h = MEDIA_H;
    media_widget.visible = 1;
    media_widget.focused = 0;
    media_widget.draggable = 1;
    media_widget.resizable = 0;
    media_widget.draw = media_draw;
    media_widget.on_event = media_on_event;
    media_widget.state = &media_state;
    memcpy(media_widget.title, "Media", 6);
    return &media_widget;
}
