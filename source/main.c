#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TONE_RATE        22050u
#define TONE_SECONDS     2u
#define TONE_FRAMES      (TONE_RATE * TONE_SECONDS)
#define TONE_FREQ        440.0
#define TONE_AMPLITUDE   7000.0

#define CAP_RATE         32728u
#define CAP_SECONDS      1u
#define CAP_SAMPLES      (CAP_RATE * CAP_SECONDS)
#define CAP_BYTES        (CAP_SAMPLES * sizeof(s16))

typedef struct {
    bool attempted;
    bool bit2;
    Result csndInitRc;
    Result ndspInitRc;
    Result acquireRc[2];
    u32 capUnit[2];
    Result execStartRc;
    Result execStopRc;
    int peak[2];
    double rms[2];
    double freq[2];
    u32 activeSamples[2];
    bool signal[2];
} CaptureTest;

static CaptureTest tests[2]; /* [0]=bit2 off, [1]=bit2 on */
static s16 *captureBuf[2][2]; /* test, capture slot */

static void fill_tone(u32 *buffer)
{
    for (u32 i = 0; i < TONE_FRAMES; i++) {
        double p = 2.0 * M_PI * TONE_FREQ * (double)i / (double)TONE_RATE;
        s16 s = (s16)(sin(p) * TONE_AMPLITUDE);
        u16 us = (u16)s;
        buffer[i] = ((u32)us << 16) | us;
    }
}

static void analyze_buffer(CaptureTest *t, int slot, s16 *buf)
{
    int peak = 0;
    long double sumsq = 0.0L;
    u32 active = 0;

    const int threshold = 32;
    u32 first = CAP_SAMPLES;
    u32 last = 0;

    for (u32 i = 0; i < CAP_SAMPLES; i++) {
        int v = (int)buf[i];
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sumsq += (long double)v * (long double)v;

        if (a > threshold) {
            if (first == CAP_SAMPLES) first = i;
            last = i;
            active++;
        }
    }

    double rms = sqrt((double)(sumsq / (long double)CAP_SAMPLES));
    double hz = 0.0;

    if (first < CAP_SAMPLES && last > first) {
        u32 crossings = 0;
        s16 prev = buf[first];
        for (u32 i = first + 1; i <= last; i++) {
            s16 cur = buf[i];
            if (prev <= 0 && cur > 0)
                crossings++;
            prev = cur;
        }
        double seconds = (double)(last - first) / (double)CAP_RATE;
        if (seconds > 0.0)
            hz = (double)crossings / seconds;
    }

    t->peak[slot] = peak;
    t->rms[slot] = rms;
    t->freq[slot] = hz;
    t->activeSamples[slot] = active;

    /* Broad threshold: we care whether NDSP signal is visible at all. */
    t->signal[slot] = (peak > 128 && active > 500);
}

static void configure_ndsp_tone(u32 *tone, ndspWaveBuf *wave)
{
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, TONE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    memset(wave, 0, sizeof(*wave));
    wave->data_vaddr = tone;
    wave->nsamples = TONE_FRAMES;

    DSP_FlushDataCache(tone, TONE_FRAMES * sizeof(u32));
    ndspChnWaveBufAdd(0, wave);
}

static void run_test(int index, bool bit2)
{
    CaptureTest *t = &tests[index];
    memset(t, 0, sizeof(*t));
    t->attempted = true;
    t->bit2 = bit2;
    t->capUnit[0] = 0xFFFFFFFFu;
    t->capUnit[1] = 0xFFFFFFFFu;
    t->execStartRc = (Result)-1;
    t->execStopRc = (Result)-1;

    u32 *tone = (u32*)linearAlloc(TONE_FRAMES * sizeof(u32));
    if (!tone)
        return;
    fill_tone(tone);

    for (int i = 0; i < 2; i++) {
        if (captureBuf[index][i])
            linearFree(captureBuf[index][i]);
        captureBuf[index][i] = (s16*)linearAlloc(CAP_BYTES);
        if (captureBuf[index][i])
            memset(captureBuf[index][i], 0, CAP_BYTES);
    }

    if (!captureBuf[index][0] || !captureBuf[index][1]) {
        linearFree(tone);
        return;
    }

    t->csndInitRc = csndInit();
    if (R_FAILED(t->csndInitRc)) {
        linearFree(tone);
        return;
    }

    t->acquireRc[0] = CSND_AcquireCapUnit(&t->capUnit[0]);
    t->acquireRc[1] = CSND_AcquireCapUnit(&t->capUnit[1]);

    t->ndspInitRc = ndspInit();
    if (R_FAILED(t->ndspInitRc)) {
        if (R_SUCCEEDED(t->acquireRc[0])) CSND_ReleaseCapUnit(t->capUnit[0]);
        if (R_SUCCEEDED(t->acquireRc[1])) CSND_ReleaseCapUnit(t->capUnit[1]);
        csndExit();
        linearFree(tone);
        return;
    }

    ndspWaveBuf wave;
    configure_ndsp_tone(tone, &wave);

    /* Let NDSP/DSP/I2S1 settle before starting the CSND capture hardware. */
    svcSleepThread(100LL * 1000LL * 1000LL);

    for (int i = 0; i < 2; i++) {
        if (R_FAILED(t->acquireRc[i]))
            continue;

        u32 phys = osConvertVirtToPhys(captureBuf[index][i]);

        /* Use the public libctru commands separately so there is no ambiguity
           about the packed flags layout of CSND_SetCapRegs(). */
        CSND_CapEnable(t->capUnit[i], false);
        CSND_CapSetRepeat(t->capUnit[i], false);   /* one-shot */
        CSND_CapSetFormat(t->capUnit[i], false);  /* PCM16 */
        CSND_CapSetBit2(t->capUnit[i], bit2);
        CSND_CapSetTimer(t->capUnit[i], CSND_TIMER(CAP_RATE));
        CSND_CapSetBuffer(t->capUnit[i], phys, CAP_BYTES);
        CSND_CapEnable(t->capUnit[i], true);
    }

    t->execStartRc = csndExecCmds(true);

    /* One second capture, with margin. */
    svcSleepThread(1150LL * 1000LL * 1000LL);

    for (int i = 0; i < 2; i++) {
        if (R_SUCCEEDED(t->acquireRc[i]))
            CSND_CapEnable(t->capUnit[i], false);
    }
    t->execStopRc = csndExecCmds(true);

    for (int i = 0; i < 2; i++) {
        if (R_FAILED(t->acquireRc[i]))
            continue;

        CSND_InvalidateDataCache(captureBuf[index][i], CAP_BYTES);
        analyze_buffer(t, i, captureBuf[index][i]);
    }

    ndspChnReset(0);
    ndspExit();

    for (int i = 0; i < 2; i++) {
        if (R_SUCCEEDED(t->acquireRc[i]))
            CSND_ReleaseCapUnit(t->capUnit[i]);
    }

    csndExit();
    linearFree(tone);
}

static void write_u16_le(FILE *f, u16 v)
{
    u8 b[2] = {(u8)v, (u8)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void write_u32_le(FILE *f, u32 v)
{
    u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static bool save_wav(const char *path, const s16 *data)
{
    if (!data) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    const u32 dataBytes = CAP_BYTES;
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + dataBytes);
    fwrite("WAVEfmt ", 1, 8, f);
    write_u32_le(f, 16);
    write_u16_le(f, 1);
    write_u16_le(f, 1);
    write_u32_le(f, CAP_RATE);
    write_u32_le(f, CAP_RATE * 2);
    write_u16_le(f, 2);
    write_u16_le(f, 16);
    fwrite("data", 1, 4, f);
    write_u32_le(f, dataBytes);
    fwrite(data, 1, dataBytes, f);
    fclose(f);
    return true;
}

static bool save_log(void)
{
    FILE *f = fopen("sdmc:/3ds_audio_csnd_capture.txt", "w");
    if (!f) return false;

    fprintf(f, "3DS Audio CSND Capture Probe v0.7\n");
    fprintf(f, "Goal: detect whether CSND capture hardware can see NDSP/I2S1 final output.\n");
    fprintf(f, "NDSP_TONE=440Hz capture_rate=%u capture_seconds=%u\n\n",
            CAP_RATE, CAP_SECONDS);

    for (int ti = 0; ti < 2; ti++) {
        CaptureTest *t = &tests[ti];
        fprintf(f, "=== BIT2_%d ===\n", t->bit2 ? 1 : 0);
        fprintf(f, "ATTEMPTED=%u\n", t->attempted ? 1 : 0);
        fprintf(f, "CSND_INIT=0x%08lX\n", (unsigned long)t->csndInitRc);
        fprintf(f, "NDSP_INIT=0x%08lX\n", (unsigned long)t->ndspInitRc);
        fprintf(f, "EXEC_START=0x%08lX EXEC_STOP=0x%08lX\n",
                (unsigned long)t->execStartRc, (unsigned long)t->execStopRc);

        for (int i = 0; i < 2; i++) {
            fprintf(f,
                "SLOT%d acquire=0x%08lX unit=%lu peak=%d rms=%.3f active=%lu freq=%.3f signal=%u\n",
                i,
                (unsigned long)t->acquireRc[i],
                (unsigned long)t->capUnit[i],
                t->peak[i],
                t->rms[i],
                (unsigned long)t->activeSamples[i],
                t->freq[i],
                t->signal[i] ? 1 : 0);
        }
        fprintf(f, "\n");
    }

    bool any = false;
    for (int ti = 0; ti < 2; ti++)
        for (int i = 0; i < 2; i++)
            any |= tests[ti].signal[i];

    fprintf(f, "=== RESULT ===\n");
    fprintf(f, "ANY_CSND_CAPTURE_SEES_NDSP_SIGNAL=%u\n", any ? 1 : 0);
    fprintf(f, "If any slot reports frequency near 440Hz, a TwlBg-only hardware bridge is plausible.\n");
    fprintf(f, "If all slots are silent/noise, use ARM7/nds-bootstrap mixer capture instead.\n");

    fclose(f);

    save_wav("sdmc:/csnd_cap_b0_s0.wav", captureBuf[0][0]);
    save_wav("sdmc:/csnd_cap_b0_s1.wav", captureBuf[0][1]);
    save_wav("sdmc:/csnd_cap_b1_s0.wav", captureBuf[1][0]);
    save_wav("sdmc:/csnd_cap_b1_s1.wav", captureBuf[1][1]);

    return true;
}

static void draw(void)
{
    printf("\x1b[2J\x1b[H");
    printf("3DS Audio CSND Capture Probe v0.7\n\n");
    printf("Tests if CSND capture can see NDSP/I2S1.\n\n");

    printf("[X] Test both capture units, bit2=0\n");
    if (tests[0].attempted) {
        printf("    U%lu: peak=%d rms=%.1f hz=%.1f %s\n",
               (unsigned long)tests[0].capUnit[0], tests[0].peak[0],
               tests[0].rms[0], tests[0].freq[0],
               tests[0].signal[0] ? "SIGNAL" : "no");
        printf("    U%lu: peak=%d rms=%.1f hz=%.1f %s\n",
               (unsigned long)tests[0].capUnit[1], tests[0].peak[1],
               tests[0].rms[1], tests[0].freq[1],
               tests[0].signal[1] ? "SIGNAL" : "no");
    }

    printf("\n[Y] Test both capture units, bit2=1\n");
    if (tests[1].attempted) {
        printf("    U%lu: peak=%d rms=%.1f hz=%.1f %s\n",
               (unsigned long)tests[1].capUnit[0], tests[1].peak[0],
               tests[1].rms[0], tests[1].freq[0],
               tests[1].signal[0] ? "SIGNAL" : "no");
        printf("    U%lu: peak=%d rms=%.1f hz=%.1f %s\n",
               (unsigned long)tests[1].capUnit[1], tests[1].peak[1],
               tests[1].rms[1], tests[1].freq[1],
               tests[1].signal[1] ? "SIGNAL" : "no");
    }

    printf("\n[B] Save log + four WAV files\n");
    printf("[START] Exit\n\n");
    printf("Run X, then Y, then B.\n");
    printf("NDSP tone itself may remain inaudible; that's expected.\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    draw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START)
            break;
        if (down & KEY_X) {
            run_test(0, false);
            draw();
        }
        if (down & KEY_Y) {
            run_test(1, true);
            draw();
        }
        if (down & KEY_B) {
            bool ok = save_log();
            draw();
            printf("\nlog/WAV: %s\n", ok ? "saved" : "FAILED");
        }

        gspWaitForVBlank();
    }

    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 2; i++)
            if (captureBuf[t][i])
                linearFree(captureBuf[t][i]);

    gfxExit();
    return 0;
}
