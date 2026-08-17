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
// Deliberately low digital amplitude. Y forces the I2S1 DAC volume to 0 dB.
#define TONE_AMPLITUDE  2500.0

typedef struct {
    u8 i2s;
    u8 dsp;
} PdnSnapshot;

typedef struct {
    bool valid;
    Result rc;
    PdnSnapshot during;
    PdnSnapshot after;
} SimpleTestResult;

typedef struct {
    bool valid;
    bool forceI2s1;
    Result ndspRc;
    Result cdcRc;
    Result setVolumeRc;
    bool cdcOpened;
    bool volumeWriteAttempted;
    bool queued;
    u16 finalWaveStatus;
    PdnSnapshot during;
    PdnSnapshot after;
} ToneTestResult;

static PdnSnapshot startup;
static SimpleTestResult csndTest;
static ToneTestResult normalToneTest;
static ToneTestResult forcedToneTest;
static bool volumeWasForced = false;

static PdnSnapshot read_pdn(void)
{
    PdnSnapshot s;
    s.i2s = REG8_RO(PDN_I2S_CNT_ADDR);
    s.dsp = REG8_RO(PDN_DSP_CNT_ADDR);
    return s;
}

static const char *onoff(bool v)
{
    return v ? "ON" : "OFF";
}

static void print_snapshot(const char *label, PdnSnapshot s)
{
    printf("%s\n", label);
    printf("  PDN_I2S_CNT: 0x%02X\n", s.i2s);
    printf("    bit0 I2S1 clock*: %s\n", onoff((s.i2s & BIT(0)) != 0));
    printf("    bit1 I2S2 clock : %s\n", onoff((s.i2s & BIT(1)) != 0));
    printf("  PDN_DSP_CNT: 0x%02X\n", s.dsp);
    printf("    bit0 DSP out-reset: %s\n", onoff((s.dsp & BIT(0)) != 0));
    printf("    bit1 DSP clock    : %s\n", onoff((s.dsp & BIT(1)) != 0));
}

static void print_tone_result(const char *label, const ToneTestResult *t)
{
    if (!t->valid) {
        printf("%s\n", label);
        return;
    }

    printf("%s\n", label);
    printf("  ndspInit: 0x%08lX %s\n", (unsigned long)t->ndspRc,
           R_SUCCEEDED(t->ndspRc) ? "OK" : "FAIL");

    if (t->forceI2s1) {
        printf("  cdcChkInit: 0x%08lX %s\n", (unsigned long)t->cdcRc,
               R_SUCCEEDED(t->cdcRc) ? "OK" : "FAIL");
        if (t->volumeWriteAttempted) {
            printf("  I2S1 volume -> 0 dB: 0x%08lX %s\n",
                   (unsigned long)t->setVolumeRc,
                   R_SUCCEEDED(t->setVolumeRc) ? "OK" : "FAIL");
        }
    }

    printf("  Tone queued: %s   wave status: %u\n",
           t->queued ? "YES" : "NO", (unsigned)t->finalWaveStatus);
    print_snapshot("  during test", t->during);
}

static void draw(void)
{
    printf("\x1b[2J\x1b[H");
    printf("3DS Audio I2S Diagnostic v0.3\n");
    printf("DSP/I2S1 volume test\n\n");

    print_snapshot("STARTUP", startup);
    printf("\n");

    if (csndTest.valid) {
        printf("[A] CSND init: 0x%08lX %s\n", (unsigned long)csndTest.rc,
               R_SUCCEEDED(csndTest.rc) ? "OK" : "FAIL");
    } else {
        printf("[A] Test CSND / I2S2 init\n");
    }

    printf("\n");
    print_tone_result("[X] NDSP tone NORMAL", &normalToneTest);
    printf("\n");
    print_tone_result("[Y] FORCE I2S1=0dB + NDSP tone", &forcedToneTest);

    printf("\n[B] Save report to SD\n");
    printf("[START] Exit\n");
    printf("\nY WRITES codec I2S1 DAC volume to 0 dB.\n");
    printf("Tone level is intentionally low. Reboot after test.\n");
    printf("* I2S1 PDN bit0 is documented as uncertain.\n");

    if (volumeWasForced)
        printf("\nI2S1 0 dB write succeeded during this run.\n");
}

static void run_csnd_test(void)
{
    memset(&csndTest, 0, sizeof(csndTest));
    csndTest.valid = true;
    csndTest.rc = csndInit();
    csndTest.during = read_pdn();
    if (R_SUCCEEDED(csndTest.rc))
        csndExit();
    svcSleepThread(20 * 1000 * 1000LL);
    csndTest.after = read_pdn();
}

static void fill_stereo_tone(u32 *buffer)
{
    for (size_t i = 0; i < TONE_FRAMES; i++) {
        double phase = (2.0 * M_PI * TONE_FREQ * (double)i) / (double)TONE_RATE;
        s16 sample = (s16)(sin(phase) * TONE_AMPLITUDE);
        u16 usample = (u16)sample;
        buffer[i] = ((u32)usample << 16) | (u32)usample;
    }
}

static void run_tone_test(ToneTestResult *out, bool forceI2s1)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->forceI2s1 = forceI2s1;
    out->cdcRc = (Result)-1;
    out->setVolumeRc = (Result)-1;

    const size_t bufferBytes = TONE_FRAMES * sizeof(u32);
    u32 *audioBuffer = (u32 *)linearAlloc(bufferBytes);
    if (!audioBuffer) {
        out->ndspRc = (Result)-1;
        out->during = read_pdn();
        out->after = out->during;
        return;
    }

    fill_stereo_tone(audioBuffer);
    DSP_FlushDataCache(audioBuffer, bufferBytes);

    out->ndspRc = ndspInit();
    if (R_FAILED(out->ndspRc)) {
        out->during = read_pdn();
        out->after = out->during;
        linearFree(audioBuffer);
        return;
    }

    // Force the primary I2S DAC path only after NDSP/DSP is active.
    if (forceI2s1) {
        out->cdcRc = cdcChkInit();
        if (R_SUCCEEDED(out->cdcRc)) {
            out->cdcOpened = true;
            out->volumeWriteAttempted = true;
            out->setVolumeRc = CDCCHK_SetI2sVolume(CODEC_I2S_LINE_1, 0);
            if (R_SUCCEEDED(out->setVolumeRc))
                volumeWasForced = true;
        }
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

    ndspWaveBuf waveBuf;
    memset(&waveBuf, 0, sizeof(waveBuf));
    waveBuf.data_vaddr = audioBuffer;
    waveBuf.nsamples = TONE_FRAMES;

    out->during = read_pdn();
    ndspChnWaveBufAdd(0, &waveBuf);
    out->queued = true;

    // Tone lasts 2 seconds. Keep services active while it is playing.
    svcSleepThread((TONE_SECONDS * 1000LL + 250LL) * 1000LL * 1000LL);
    out->finalWaveStatus = (u16)waveBuf.status;

    ndspChnReset(0);

    if (out->cdcOpened)
        cdcChkExit();

    ndspExit();
    svcSleepThread(20 * 1000 * 1000LL);
    out->after = read_pdn();
    linearFree(audioBuffer);
}

static void fprint_snapshot(FILE *f, const char *label, PdnSnapshot s)
{
    fprintf(f, "%s\n", label);
    fprintf(f, "PDN_I2S_CNT=0x%02X I2S1_bit0=%u I2S2_bit1=%u\n",
            s.i2s, !!(s.i2s & BIT(0)), !!(s.i2s & BIT(1)));
    fprintf(f, "PDN_DSP_CNT=0x%02X DSP_out_reset_bit0=%u DSP_clock_bit1=%u\n",
            s.dsp, !!(s.dsp & BIT(0)), !!(s.dsp & BIT(1)));
}

static void fprint_tone(FILE *f, const char *name, const ToneTestResult *t)
{
    if (!t->valid)
        return;

    fprintf(f, "\n%s\n", name);
    fprintf(f, "NDSP rc=0x%08lX (%s)\n", (unsigned long)t->ndspRc,
            R_SUCCEEDED(t->ndspRc) ? "OK" : "FAIL");
    fprintf(f, "FORCE_I2S1=%u\n", t->forceI2s1 ? 1 : 0);

    if (t->forceI2s1) {
        fprintf(f, "CDCCHK rc=0x%08lX (%s)\n", (unsigned long)t->cdcRc,
                R_SUCCEEDED(t->cdcRc) ? "OK" : "FAIL");
        fprintf(f, "I2S1_VOLUME_WRITE_ATTEMPTED=%u\n", t->volumeWriteAttempted ? 1 : 0);
        if (t->volumeWriteAttempted) {
            fprintf(f, "I2S1_SET_0DB rc=0x%08lX (%s)\n",
                    (unsigned long)t->setVolumeRc,
                    R_SUCCEEDED(t->setVolumeRc) ? "OK" : "FAIL");
        }
    }

    fprintf(f, "TONE_QUEUED=%u\n", t->queued ? 1 : 0);
    fprintf(f, "FINAL_WAVE_STATUS=%u\n", (unsigned)t->finalWaveStatus);
    fprint_snapshot(f, "TONE_DURING", t->during);
    fprint_snapshot(f, "TONE_AFTER", t->after);
}

static bool save_report(void)
{
    FILE *f = fopen("sdmc:/3ds_audio_i2s_diag.txt", "w");
    if (!f)
        return false;

    fprintf(f, "3DS Audio I2S Diagnostic v0.3\n");
    fprintf(f, "Y writes CODEC I2S1 DAC volume to 0 dB when cdc:CHK is available.\n");
    fprintf(f, "TONE=%dHz duration=%ds digital_amplitude=%.0f/32767\n\n",
            (int)TONE_FREQ, TONE_SECONDS, TONE_AMPLITUDE);

    fprint_snapshot(f, "STARTUP", startup);

    if (csndTest.valid) {
        fprintf(f, "\nCSND rc=0x%08lX (%s)\n", (unsigned long)csndTest.rc,
                R_SUCCEEDED(csndTest.rc) ? "OK" : "FAIL");
        fprint_snapshot(f, "CSND_DURING", csndTest.during);
        fprint_snapshot(f, "CSND_AFTER", csndTest.after);
    }

    fprint_tone(f, "NDSP_TONE_NORMAL", &normalToneTest);
    fprint_tone(f, "NDSP_TONE_I2S1_0DB", &forcedToneTest);
    fprintf(f, "\nI2S1_VOLUME_FORCE_SUCCEEDED=%u\n", volumeWasForced ? 1 : 0);

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
            run_csnd_test();
            draw();
        }
        if (down & KEY_X) {
            run_tone_test(&normalToneTest, false);
            draw();
        }
        if (down & KEY_Y) {
            run_tone_test(&forcedToneTest, true);
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
