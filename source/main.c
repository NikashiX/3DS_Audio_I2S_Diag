#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// Physical PDN registers documented at 0x10141220 / 0x10141230.
// In a 3DS user process, mapped IO uses +0x0EB00000 virtual offset.
#define PDN_I2S_CNT_ADDR  ((uintptr_t)0x1EC41220)
#define PDN_DSP_CNT_ADDR  ((uintptr_t)0x1EC41230)
#define REG8_RO(addr) (*(volatile const u8 *)(addr))

#define TONE_RATE       22050
#define TONE_SECONDS    2
#define TONE_FRAMES     (TONE_RATE * TONE_SECONDS)
#define TONE_FREQ       440.0
#define TONE_AMPLITUDE  5000.0

#define CODEC_REG_COUNT 128

// Curated pages: low pages + codec banks referenced by 3DS codec docs/research.
// Reads only. Each page is read in two 64-byte chunks because cdc:CHK caps
// each read command at 64 bytes.
static const u8 codecPages[] = {
    0x00, 0x01, 0x02, 0x03,
    0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x6B,
    0x6C, 0x6D, 0x6E, 0x6F,
    0xFF
};

#define CODEC_PAGE_COUNT (sizeof(codecPages) / sizeof(codecPages[0]))

typedef struct {
    u8 i2s;
    u8 dsp;
} PdnSnapshot;

typedef struct {
    Result rc0;
    Result rc40;
    bool valid0;
    bool valid40;
    u8 data[CODEC_REG_COUNT];
} CodecPageDump;

typedef struct {
    bool attempted;
    Result cdcInitRc;
    PdnSnapshot pdn;
    CodecPageDump page[CODEC_PAGE_COUNT];
} CodecSnapshot;

typedef struct {
    bool attempted;
    Result audioInitRc;
    Result playRc;
    int channel;
    PdnSnapshot pdnDuring;
    CodecSnapshot codec;
} AudioCodecTest;

static PdnSnapshot startup;
static CodecSnapshot baselineCodec;
static AudioCodecTest ndspTest;
static AudioCodecTest csndTest;

static PdnSnapshot read_pdn(void)
{
    PdnSnapshot s;
    s.i2s = REG8_RO(PDN_I2S_CNT_ADDR);
    s.dsp = REG8_RO(PDN_DSP_CNT_ADDR);
    return s;
}

static const char *okfail(Result rc)
{
    return R_SUCCEEDED(rc) ? "OK" : "FAIL";
}

static void print_pdn(PdnSnapshot s)
{
    printf("PDN I2S=0x%02X  DSP=0x%02X\n", s.i2s, s.dsp);
    printf("DSP reset=%u clock=%u  I2S2=%u\n",
           !!(s.dsp & BIT(0)),
           !!(s.dsp & BIT(1)),
           !!(s.i2s & BIT(1)));
}

static bool capture_codec(CodecSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->attempted = true;
    out->pdn = read_pdn();

    out->cdcInitRc = cdcChkInit();
    if (R_FAILED(out->cdcInitRc))
        return false;

    for (size_t i = 0; i < CODEC_PAGE_COUNT; i++) {
        CodecPageDump *p = &out->page[i];
        memset(p->data, 0xEE, sizeof(p->data));

        p->rc0 = CDCCHK_ReadRegisters2(codecPages[i], 0x00, &p->data[0x00], 64);
        p->valid0 = R_SUCCEEDED(p->rc0);

        p->rc40 = CDCCHK_ReadRegisters2(codecPages[i], 0x40, &p->data[0x40], 64);
        p->valid40 = R_SUCCEEDED(p->rc40);
    }

    cdcChkExit();
    return true;
}

static void fill_stereo_tone(u32 *buffer)
{
    for (size_t i = 0; i < TONE_FRAMES; i++) {
        double phase = (2.0 * M_PI * TONE_FREQ * (double)i) / (double)TONE_RATE;
        s16 sample = (s16)(sin(phase) * TONE_AMPLITUDE);
        u16 us = (u16)sample;
        buffer[i] = ((u32)us << 16) | (u32)us;
    }
}

static void fill_mono_tone(s16 *buffer)
{
    for (size_t i = 0; i < TONE_FRAMES; i++) {
        double phase = (2.0 * M_PI * TONE_FREQ * (double)i) / (double)TONE_RATE;
        buffer[i] = (s16)(sin(phase) * TONE_AMPLITUDE);
    }
}

static void run_ndsp_test(void)
{
    memset(&ndspTest, 0, sizeof(ndspTest));
    ndspTest.attempted = true;
    ndspTest.channel = 0;
    ndspTest.playRc = (Result)-1;

    const size_t bytes = TONE_FRAMES * sizeof(u32);
    u32 *audio = (u32 *)linearAlloc(bytes);
    if (!audio) {
        ndspTest.audioInitRc = (Result)-1;
        return;
    }

    fill_stereo_tone(audio);
    DSP_FlushDataCache(audio, bytes);

    ndspTest.audioInitRc = ndspInit();
    if (R_FAILED(ndspTest.audioInitRc)) {
        linearFree(audio);
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, TONE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    ndspWaveBuf wave;
    memset(&wave, 0, sizeof(wave));
    wave.data_vaddr = audio;
    wave.nsamples = TONE_FRAMES;

    ndspChnWaveBufAdd(0, &wave);
    ndspTest.playRc = 0;

    // Snapshot while DSP/NDSP is actively processing the tone.
    svcSleepThread(50 * 1000 * 1000LL);
    ndspTest.pdnDuring = read_pdn();
    capture_codec(&ndspTest.codec);

    svcSleepThread((TONE_SECONDS * 1000LL - 50LL + 150LL) * 1000LL * 1000LL);

    ndspChnReset(0);
    ndspExit();
    linearFree(audio);
}

static Result play_csnd_first_available(s16 *audio, u32 bytes, int *usedChannel)
{
    // csndPlaySound() returns 1 if a requested channel was not acquired.
    // Try channels until one is available.
    for (int ch = 0; ch < CSND_NUM_CHANNELS; ch++) {
        Result rc = csndPlaySound(
            ch,
            SOUND_FORMAT_16BIT | SOUND_ONE_SHOT | SOUND_LINEAR_INTERP,
            TONE_RATE,
            0.50f,
            0.0f,
            audio,
            audio,
            bytes
        );

        if (rc != 1) {
            *usedChannel = ch;
            return rc;
        }
    }

    *usedChannel = -1;
    return 1;
}

static void run_csnd_test(void)
{
    memset(&csndTest, 0, sizeof(csndTest));
    csndTest.attempted = true;
    csndTest.channel = -1;
    csndTest.playRc = (Result)-1;

    const u32 bytes = (u32)(TONE_FRAMES * sizeof(s16));
    s16 *audio = (s16 *)linearAlloc(bytes);
    if (!audio) {
        csndTest.audioInitRc = (Result)-1;
        return;
    }

    fill_mono_tone(audio);

    csndTest.audioInitRc = csndInit();
    if (R_FAILED(csndTest.audioInitRc)) {
        linearFree(audio);
        return;
    }

    GSPGPU_FlushDataCache(audio, bytes);
    csndTest.playRc = play_csnd_first_available(audio, bytes, &csndTest.channel);

    // Snapshot while CSND/I2S2 is actively playing.
    svcSleepThread(50 * 1000 * 1000LL);
    csndTest.pdnDuring = read_pdn();
    capture_codec(&csndTest.codec);

    svcSleepThread((TONE_SECONDS * 1000LL - 50LL + 150LL) * 1000LL * 1000LL);

    csndExit();
    linearFree(audio);
}

static unsigned count_valid_pages(const CodecSnapshot *s)
{
    if (!s->attempted || R_FAILED(s->cdcInitRc))
        return 0;

    unsigned n = 0;
    for (size_t i = 0; i < CODEC_PAGE_COUNT; i++) {
        if (s->page[i].valid0 && s->page[i].valid40)
            n++;
    }
    return n;
}

static void draw(void)
{
    printf("\x1b[2J\x1b[H");
    printf("3DS Audio I2S Diagnostic v0.4\n");
    printf("CODEC register comparison - READ ONLY\n\n");

    printf("STARTUP: ");
    print_pdn(startup);
    printf("\n");

    if (!baselineCodec.attempted) {
        printf("[A] Capture CODEC baseline\n");
    } else {
        printf("[A] Baseline cdc:CHK: 0x%08lX %s\n",
               (unsigned long)baselineCodec.cdcInitRc,
               okfail(baselineCodec.cdcInitRc));
        printf("    full pages read: %u/%u\n",
               count_valid_pages(&baselineCodec),
               (unsigned)CODEC_PAGE_COUNT);
    }

    printf("\n");
    if (!ndspTest.attempted) {
        printf("[X] NDSP tone + CODEC snapshot\n");
    } else {
        printf("[X] NDSP init: 0x%08lX %s  play:%s\n",
               (unsigned long)ndspTest.audioInitRc,
               okfail(ndspTest.audioInitRc),
               R_SUCCEEDED(ndspTest.playRc) ? "OK" : "FAIL");
        printf("    codec pages: %u/%u\n",
               count_valid_pages(&ndspTest.codec),
               (unsigned)CODEC_PAGE_COUNT);
    }

    printf("\n");
    if (!csndTest.attempted) {
        printf("[Y] CSND tone + CODEC snapshot\n");
    } else {
        printf("[Y] CSND init: 0x%08lX %s\n",
               (unsigned long)csndTest.audioInitRc,
               okfail(csndTest.audioInitRc));
        printf("    play rc=0x%08lX channel=%d\n",
               (unsigned long)csndTest.playRc,
               csndTest.channel);
        printf("    codec pages: %u/%u\n",
               count_valid_pages(&csndTest.codec),
               (unsigned)CODEC_PAGE_COUNT);
    }

    printf("\n[B] Save full report to SD\n");
    printf("[START] Exit\n\n");
    printf("No CDCCHK writes in v0.4.\n");
    printf("Run A, X and Y, then B.\n");
}

static void dump_hex_line(FILE *f, const u8 *data, unsigned start)
{
    fprintf(f, "%02X: ", start);
    for (unsigned i = 0; i < 16; i++)
        fprintf(f, "%02X%s", data[start + i], (i == 15) ? "" : " ");
    fprintf(f, "\n");
}

static void dump_codec_snapshot(FILE *f, const char *name, const CodecSnapshot *s)
{
    fprintf(f, "\n=== %s ===\n", name);

    if (!s->attempted) {
        fprintf(f, "NOT_CAPTURED\n");
        return;
    }

    fprintf(f, "CDCCHK_INIT rc=0x%08lX (%s)\n",
            (unsigned long)s->cdcInitRc, okfail(s->cdcInitRc));
    fprintf(f, "PDN_I2S_CNT=0x%02X PDN_DSP_CNT=0x%02X\n",
            s->pdn.i2s, s->pdn.dsp);

    if (R_FAILED(s->cdcInitRc))
        return;

    for (size_t i = 0; i < CODEC_PAGE_COUNT; i++) {
        const CodecPageDump *p = &s->page[i];

        fprintf(f, "\nPAGE 0x%02X\n", codecPages[i]);
        fprintf(f, "READ_00_3F rc=0x%08lX (%s)\n",
                (unsigned long)p->rc0, okfail(p->rc0));
        fprintf(f, "READ_40_7F rc=0x%08lX (%s)\n",
                (unsigned long)p->rc40, okfail(p->rc40));

        if (!(p->valid0 || p->valid40))
            continue;

        for (unsigned off = 0; off < CODEC_REG_COUNT; off += 16) {
            // Print a chunk if its containing 64-byte read succeeded.
            if ((off < 0x40 && p->valid0) || (off >= 0x40 && p->valid40))
                dump_hex_line(f, p->data, off);
        }
    }
}

static void dump_diff(FILE *f,
                      const char *nameA, const CodecSnapshot *a,
                      const char *nameB, const CodecSnapshot *b)
{
    fprintf(f, "\n=== DIFF %s -> %s ===\n", nameA, nameB);

    if (!a->attempted || !b->attempted ||
        R_FAILED(a->cdcInitRc) || R_FAILED(b->cdcInitRc)) {
        fprintf(f, "DIFF_UNAVAILABLE\n");
        return;
    }

    unsigned changes = 0;

    for (size_t i = 0; i < CODEC_PAGE_COUNT; i++) {
        const CodecPageDump *pa = &a->page[i];
        const CodecPageDump *pb = &b->page[i];

        for (unsigned reg = 0; reg < CODEC_REG_COUNT; reg++) {
            bool validA = reg < 0x40 ? pa->valid0 : pa->valid40;
            bool validB = reg < 0x40 ? pb->valid0 : pb->valid40;

            if (!validA || !validB)
                continue;

            if (pa->data[reg] != pb->data[reg]) {
                fprintf(f, "P%02X:R%02X %02X -> %02X\n",
                        codecPages[i], reg, pa->data[reg], pb->data[reg]);
                changes++;
            }
        }
    }

    fprintf(f, "TOTAL_CHANGED_REGISTERS=%u\n", changes);
}

static bool save_report(void)
{
    FILE *f = fopen("sdmc:/3ds_audio_i2s_diag.txt", "w");
    if (!f)
        return false;

    fprintf(f, "3DS Audio I2S Diagnostic v0.4\n");
    fprintf(f, "Codec comparison build. CDCCHK register access is READ ONLY.\n");
    fprintf(f, "TONE=%dHz duration=%ds amplitude=%.0f/32767\n",
            (int)TONE_FREQ, TONE_SECONDS, TONE_AMPLITUDE);
    fprintf(f, "CODEC_READ_API=CDCCHK_ReadRegisters2\n");
    fprintf(f, "CODEC_PAGES=");
    for (size_t i = 0; i < CODEC_PAGE_COUNT; i++)
        fprintf(f, "%02X%s", codecPages[i], (i + 1 == CODEC_PAGE_COUNT) ? "\n" : ",");

    fprintf(f, "\nSTARTUP\n");
    fprintf(f, "PDN_I2S_CNT=0x%02X I2S1_bit0=%u I2S2_bit1=%u\n",
            startup.i2s,
            !!(startup.i2s & BIT(0)),
            !!(startup.i2s & BIT(1)));
    fprintf(f, "PDN_DSP_CNT=0x%02X DSP_out_reset_bit0=%u DSP_clock_bit1=%u\n",
            startup.dsp,
            !!(startup.dsp & BIT(0)),
            !!(startup.dsp & BIT(1)));

    fprintf(f, "\nNDSP_TEST_ATTEMPTED=%u\n", ndspTest.attempted ? 1 : 0);
    if (ndspTest.attempted) {
        fprintf(f, "NDSP_INIT rc=0x%08lX (%s)\n",
                (unsigned long)ndspTest.audioInitRc, okfail(ndspTest.audioInitRc));
        fprintf(f, "NDSP_PLAY rc=0x%08lX (%s)\n",
                (unsigned long)ndspTest.playRc, okfail(ndspTest.playRc));
        fprintf(f, "NDSP_PDN_DURING I2S=0x%02X DSP=0x%02X\n",
                ndspTest.pdnDuring.i2s, ndspTest.pdnDuring.dsp);
    }

    fprintf(f, "\nCSND_TEST_ATTEMPTED=%u\n", csndTest.attempted ? 1 : 0);
    if (csndTest.attempted) {
        fprintf(f, "CSND_INIT rc=0x%08lX (%s)\n",
                (unsigned long)csndTest.audioInitRc, okfail(csndTest.audioInitRc));
        fprintf(f, "CSND_PLAY rc=0x%08lX (%s)\n",
                (unsigned long)csndTest.playRc, okfail(csndTest.playRc));
        fprintf(f, "CSND_CHANNEL=%d\n", csndTest.channel);
        fprintf(f, "CSND_PDN_DURING I2S=0x%02X DSP=0x%02X\n",
                csndTest.pdnDuring.i2s, csndTest.pdnDuring.dsp);
    }

    dump_codec_snapshot(f, "CODEC_BASELINE", &baselineCodec);
    dump_codec_snapshot(f, "CODEC_DURING_NDSP", &ndspTest.codec);
    dump_codec_snapshot(f, "CODEC_DURING_CSND", &csndTest.codec);

    dump_diff(f, "BASELINE", &baselineCodec, "NDSP", &ndspTest.codec);
    dump_diff(f, "BASELINE", &baselineCodec, "CSND", &csndTest.codec);
    dump_diff(f, "NDSP", &ndspTest.codec, "CSND", &csndTest.codec);

    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    startup = read_pdn();
    draw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START)
            break;

        if (down & KEY_A) {
            capture_codec(&baselineCodec);
            draw();
        }

        if (down & KEY_X) {
            run_ndsp_test();
            draw();
        }

        if (down & KEY_Y) {
            run_csnd_test();
            draw();
        }

        if (down & KEY_B) {
            bool ok = save_report();
            draw();
            printf("\nReport: %s\n",
                   ok ? "sdmc:/3ds_audio_i2s_diag.txt" : "FAILED TO SAVE");
        }

        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
