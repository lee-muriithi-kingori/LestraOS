/*
 * Lestra OS - Text-to-Speech Engine (Formant Synthesis)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A REAL text-to-speech engine using formant synthesis. Converts text
 * to phonemes, synthesizes each as sum of formant sine waves, outputs
 * PCM audio through AC97. Sounds robotic but WORKS.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

extern int ac97_is_present(void);
extern int ac97_play(const void* buf, uint32_t len);

#define TTS_SAMPLE_RATE 22050
#define TTS_MAX_SAMPLES (TTS_SAMPLE_RATE * 10)

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

static int16_t tts_sin(uint32_t phase) { return sin_table[phase & 0xFF]; }

static uint32_t noise_state = 0x12345678;
static int16_t tts_noise(void) {
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return (noise_state & 0x3FFF) - 0x2000;
}

struct phoneme {
    const char* code;
    int f1, f2, f3;
    int duration_ms;
    int voiced;
    int amplitude;
};

static const struct phoneme phonemes[] = {
    {"AH", 730, 1090, 2440, 120, 1, 80},
    {"AA", 570, 840, 2410, 120, 1, 80},
    {"AE", 660, 1720, 2410, 100, 1, 75},
    {"EH", 530, 1840, 2480, 100, 1, 75},
    {"IH", 390, 1990, 2550, 80, 1, 70},
    {"IY", 270, 2290, 3010, 100, 1, 75},
    {"UH", 440, 1020, 2240, 80, 1, 70},
    {"UW", 300, 870, 2240, 120, 1, 80},
    {"OW", 570, 840, 2410, 120, 1, 80},
    {"AY", 660, 1720, 2410, 150, 1, 75},
    {"AW", 570, 840, 2410, 150, 1, 75},
    {"OY", 440, 1020, 2240, 150, 1, 75},
    {"ER", 490, 1350, 1690, 100, 1, 75},
    {"B", 200, 800, 2200, 50, 1, 60},
    {"D", 200, 800, 2200, 50, 1, 60},
    {"G", 200, 800, 2200, 50, 1, 60},
    {"M", 280, 1000, 2400, 80, 1, 65},
    {"N", 280, 1000, 2400, 80, 1, 65},
    {"NG", 280, 1200, 2400, 80, 1, 65},
    {"L", 300, 1100, 2400, 80, 1, 65},
    {"R", 330, 1200, 2400, 80, 1, 65},
    {"W", 300, 700, 2200, 60, 1, 60},
    {"Y", 300, 2000, 2600, 60, 1, 60},
    {"V", 300, 1100, 2400, 60, 1, 55},
    {"TH", 400, 1400, 2400, 60, 1, 55},
    {"Z", 300, 1400, 2400, 60, 1, 55},
    {"ZH", 350, 1500, 2500, 60, 1, 55},
    {"P", 0, 0, 0, 50, 0, 50},
    {"T", 0, 0, 0, 50, 0, 50},
    {"K", 0, 0, 0, 50, 0, 50},
    {"F", 0, 0, 0, 80, 0, 45},
    {"S", 0, 0, 0, 100, 0, 50},
    {"SH", 0, 0, 0, 100, 0, 45},
    {"H", 0, 0, 0, 80, 0, 45},
    {"CH", 0, 0, 0, 80, 0, 50},
    {"TH2", 0, 0, 0, 80, 0, 45},
    {"_", 0, 0, 0, 50, 0, 0},
};
#define NUM_PHONEMES (sizeof(phonemes)/sizeof(phonemes[0]))

#define MAX_PHONEMES 256
static int phoneme_seq[MAX_PHONEMES];
static int phoneme_count = 0;

static void add_phoneme(int idx) {
    if (phoneme_count < MAX_PHONEMES) phoneme_seq[phoneme_count++] = idx;
}

static int find_phoneme(const char* code) {
    for (int i = 0; i < (int)NUM_PHONEMES; i++)
        if (strcmp(phonemes[i].code, code) == 0) return i;
    return -1;
}

static void text_to_phonemes(const char* text) {
    phoneme_count = 0;
    int i = 0;
    while (text[i] && phoneme_count < MAX_PHONEMES - 1) {
        char c = text[i], next = text[i+1], next2 = text[i+2];
        if (c < 'a' || c > 'z') {
            if (c == ' ' || c == ',' || c == '.') add_phoneme(find_phoneme("_"));
            i++; continue;
        }
        if (c == 't' && next == 'h' && next2 == 'e') { add_phoneme(find_phoneme("TH")); i += 3; continue; }
        if (c == 't' && next == 'h') { add_phoneme(find_phoneme("TH2")); i += 2; continue; }
        if (c == 's' && next == 'h') { add_phoneme(find_phoneme("SH")); i += 2; continue; }
        if (c == 'c' && next == 'h') { add_phoneme(find_phoneme("CH")); i += 2; continue; }
        if (c == 'n' && next == 'g') { add_phoneme(find_phoneme("NG")); i += 2; continue; }
        if (c == 'q' && next == 'u') { add_phoneme(find_phoneme("K")); add_phoneme(find_phoneme("W")); i += 2; continue; }

        switch (c) {
            case 'b': add_phoneme(find_phoneme("B")); break;
            case 'c': add_phoneme(find_phoneme(next == 'e' || next == 'i' || next == 'y' ? "S" : "K")); break;
            case 'd': add_phoneme(find_phoneme("D")); break;
            case 'f': add_phoneme(find_phoneme("F")); break;
            case 'g': add_phoneme(find_phoneme("G")); break;
            case 'h': add_phoneme(find_phoneme("H")); break;
            case 'j': add_phoneme(find_phoneme("CH")); break;
            case 'k': add_phoneme(find_phoneme("K")); break;
            case 'l': add_phoneme(find_phoneme("L")); break;
            case 'm': add_phoneme(find_phoneme("M")); break;
            case 'n': add_phoneme(find_phoneme("N")); break;
            case 'p': add_phoneme(find_phoneme("P")); break;
            case 'r': add_phoneme(find_phoneme("R")); break;
            case 's': add_phoneme(find_phoneme("S")); break;
            case 't': add_phoneme(find_phoneme("T")); break;
            case 'v': add_phoneme(find_phoneme("V")); break;
            case 'w': add_phoneme(find_phoneme("W")); break;
            case 'x': add_phoneme(find_phoneme("K")); add_phoneme(find_phoneme("S")); break;
            case 'y': add_phoneme(find_phoneme("Y")); break;
            case 'z': add_phoneme(find_phoneme("Z")); break;
            case 'a':
                if (next == 'y' || next == 'i') { add_phoneme(find_phoneme("AY")); i += 2; continue; }
                if (next == 'w') { add_phoneme(find_phoneme("AW")); i += 2; continue; }
                if (next == 'r') { add_phoneme(find_phoneme("ER")); i += 2; continue; }
                add_phoneme(find_phoneme("AE")); break;
            case 'e':
                if (next == 'e') { add_phoneme(find_phoneme("IY")); i += 2; continue; }
                if (next == 'w') { add_phoneme(find_phoneme("UW")); i += 2; continue; }
                if (next == 'r') { add_phoneme(find_phoneme("ER")); i += 2; continue; }
                add_phoneme(find_phoneme("EH")); break;
            case 'i':
                if (next == 'e') { add_phoneme(find_phoneme("IY")); i += 2; continue; }
                if (next == 'r') { add_phoneme(find_phoneme("ER")); i += 2; continue; }
                add_phoneme(find_phoneme("IH")); break;
            case 'o':
                if (next == 'o' || next == 'u') { add_phoneme(find_phoneme("UW")); i += 2; continue; }
                if (next == 'w') { add_phoneme(find_phoneme("OW")); i += 2; continue; }
                if (next == 'y') { add_phoneme(find_phoneme("OY")); i += 2; continue; }
                if (next == 'r') { add_phoneme(find_phoneme("OW")); i += 2; continue; }
                add_phoneme(find_phoneme("OW")); break;
            case 'u':
                if (next == 'r') { add_phoneme(find_phoneme("ER")); i += 2; continue; }
                add_phoneme(find_phoneme("UH")); break;
        }
        i++;
    }
}

static int synthesize_phoneme(int phon_idx, int16_t* out, int max_samples, int base_pitch) {
    const struct phoneme* p = &phonemes[phon_idx];
    int num_samples = (p->duration_ms * TTS_SAMPLE_RATE) / 1000;
    if (num_samples > max_samples) num_samples = max_samples;

    if (p->amplitude == 0) { memset(out, 0, num_samples * sizeof(int16_t)); return num_samples; }

    if (!p->voiced) {
        for (int i = 0; i < num_samples; i++) {
            int16_t n = tts_noise();
            int env = p->amplitude;
            if (i < num_samples / 10) env = env * i / (num_samples / 10 + 1);
            if (i > num_samples * 9 / 10) env = env * (num_samples - i) / (num_samples / 10 + 1);
            out[i] = (n * env) / 127;
        }
        return num_samples;
    }

    uint32_t phase1 = 0, phase2 = 0, phase3 = 0, pitch_phase = 0;
    int pitch = base_pitch;

    for (int i = 0; i < num_samples; i++) {
        if (i % (TTS_SAMPLE_RATE / 50) == 0 && i > 0 && pitch > base_pitch - 20) pitch--;
        uint32_t pitch_inc = (pitch * 256) / TTS_SAMPLE_RATE;
        pitch_phase += pitch_inc;
        int16_t glottal = tts_sin(pitch_phase);
        phase1 += (p->f1 * 256) / TTS_SAMPLE_RATE;
        phase2 += (p->f2 * 256) / TTS_SAMPLE_RATE;
        phase3 += (p->f3 * 256) / TTS_SAMPLE_RATE;
        int32_t sample = 0;
        sample += (glottal * tts_sin(phase1)) / 256;
        sample += (glottal * tts_sin(phase2)) / 512;
        sample += (glottal * tts_sin(phase3)) / 1024;
        int env = p->amplitude;
        int attack = num_samples / 8;
        int release = num_samples / 6;
        if (i < attack) env = env * i / (attack + 1);
        if (i > num_samples - release) env = env * (num_samples - i) / (release + 1);
        out[i] = (int16_t)((sample * env) / 127);
    }
    return num_samples;
}

int tts_speak(const char* text) {
    if (!ac97_is_present()) { pr_warn("tts: no audio device\n"); return -1; }
    if (!text || !text[0]) return -1;

    pr_info("tts: speaking \"%s\"\n", text);

    static char lower_text[512];
    int tlen = strlen(text);
    if (tlen >= (int)sizeof(lower_text)) tlen = sizeof(lower_text) - 1;
    for (int i = 0; i < tlen; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        lower_text[i] = c;
    }
    lower_text[tlen] = '\0';

    text_to_phonemes(lower_text);
    pr_info("tts: %d phonemes\n", phoneme_count);

    static int16_t audio_buf[TTS_MAX_SAMPLES];
    int total_samples = 0;
    int base_pitch = 120;

    for (int i = 0; i < phoneme_count && total_samples < TTS_MAX_SAMPLES - 1000; i++) {
        int n = synthesize_phoneme(phoneme_seq[i], &audio_buf[total_samples],
                                    TTS_MAX_SAMPLES - total_samples, base_pitch);
        total_samples += n;
    }

    pr_info("tts: synthesized %d samples (%u ms)\n",
            total_samples, (unsigned)(total_samples * 1000 / TTS_SAMPLE_RATE));

    int played = ac97_play(audio_buf, total_samples * sizeof(int16_t));
    pr_info("tts: played %d bytes\n", played);
    return 0;
}
