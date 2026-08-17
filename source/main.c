#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define PDN_DSP_CNT_ADDR ((uintptr_t)0x1EC41230)
#define I2S1_CNT_ADDR    ((uintptr_t)0x1EC45000)
#define I2S2_CNT_ADDR    ((uintptr_t)0x1EC45002)

#define REG8_RO(addr)  (*(volatile const u8  *)(addr))
#define REG16_RO(addr) (*(volatile const u16 *)(addr))

#define INPUT_RATE       22050
#define INPUT_SECONDS    2
#define INPUT_FRAMES     (INPUT_RATE * INPUT_SECONDS)
#define TONE_FREQ        440.0
#define TONE_AMPLITUDE   7000.0

// NDSP capture is the final stereo PCM output at NDSP's native output rate.
// Allocate enough room to avoid wrapping while a 2-second tone is captured.
#define CAPTURE_SECONDS  3
#define CAPTURE_RATE     ((u32)(NDSP_SAMPLE_RATE + 0.5))
#define CAPTURE_FRAMES   ((u32)(CAPTURE_RATE * CAPTURE_SECONDS))

typedef struct {
    u8  pdnDsp;
    u16 i2s1;
    u16 i2s2;
} HwSnapshot;

typedef struct {
    bool attempted;
    Result ndspRc;
    u32 capturedFrames;
    u32 firstActiveFrame;
    u32 lastActiveFrame;
    u32 nonzeroFrames;
    int peakL;
    int peakR;
    double rmsL;
    double rmsR;
    double estimatedHz;
    u32 droppedFrames;
    HwSnapshot during;
} CaptureResult;

typedef struct {
    bool attempted;
    Result csndRc;
    Result playRc;
    int channel;
    u32 framesPlayed;
    HwSnapshot during;
} ReplayResult;

static CaptureResult gCapture;
static ReplayResult gReplay;

static s16 *gCapturePcm = NULL;   // interleaved stereo: L,R,L,R...
static s16 *gReplayMono = NULL;   // left channel extracted from capture
static u32 gReplayFrames = 0;

static HwSnapshot read_hw(void)
{
    HwSnapshot s;
    s.pdnDsp = REG8_RO(PDN_DSP_CNT_ADDR);
    s.i2s1 = REG16_RO(I2S1_CNT_ADDR);
    s.i2s2 = REG16_RO(I2S2_CNT_ADDR);
    return s;
}

static int abs16_to_int(s16 v)
{
    int x = (int)v;
    return x < 0 ? -x : x;
}

static void fill_input_tone(u32 *buffer)
{
    for (u32 i = 0; i < INPUT_FRAMES; i++) {
        double phase = (2.0 * M_PI * TONE_FREQ * (double)i) / (double)INPUT_RATE;
        s16 sample = (s16)(sin(phase) * TONE_AMPLITUDE);
        u16 us = (u16)sample;
        buffer[i] = ((u32)us << 16) | (u32)us;
    }
}

static void analyze_capture(u32 frames)
{
    gCapture.capturedFrames = frames;
    gCapture.firstActiveFrame = frames;
    gCapture.lastActiveFrame = 0;
    gCapture.nonzeroFrames = 0;
    gCapture.peakL = 0;
    gCapture.peakR = 0;
    gCapture.rmsL = 0.0;
    gCapture.rmsR = 0.0;
    gCapture.estimatedHz = 0.0;

    if (!gCapturePcm || frames == 0)
        return;

    const int activeThreshold = 32;
    long double sumSqL = 0.0L;
    long double sumSqR = 0.0L;

    for (u32 i = 0; i < frames; i++) {
        s16 l = gCapturePcm[i * 2 + 0];
        s16 r = gCapturePcm[i * 2 + 1];

        int al = abs16_to_int(l);
        int ar = abs16_to_int(r);

        if (al > gCapture.peakL) gCapture.peakL = al;
        if (ar > gCapture.peakR) gCapture.peakR = ar;

        sumSqL += (long double)l * (long double)l;
        sumSqR += (long double)r * (long double)r;

        if (al > activeThreshold || ar > activeThreshold) {
            if (gCapture.firstActiveFrame == frames)
                gCapture.firstActiveFrame = i;
            gCapture.lastActiveFrame = i;
            gCapture.nonzeroFrames++;
        }
    }

    gCapture.rmsL = sqrt((double)(sumSqL / (long double)frames));
    gCapture.rmsR = sqrt((double)(sumSqR / (long double)frames));

    if (gCapture.firstActiveFrame < frames &&
        gCapture.lastActiveFrame > gCapture.firstActiveFrame) {

        u32 crossings = 0;
        s16 prev = gCapturePcm[gCapture.firstActiveFrame * 2];

        for (u32 i = gCapture.firstActiveFrame + 1;
             i <= gCapture.lastActiveFrame; i++) {
            s16 cur = gCapturePcm[i * 2];
            if (prev <= 0 && cur > 0)
                crossings++;
            prev = cur;
        }

        double duration = (double)(gCapture.lastActiveFrame -
                                   gCapture.firstActiveFrame) /
                          (double)CAPTURE_RATE;

        if (duration > 0.0)
            gCapture.estimatedHz = (double)crossings / duration;
    }
}

static void prepare_replay_buffer(void)
{
    if (gReplayMono) {
        linearFree(gReplayMono);
        gReplayMono = NULL;
    }
    gReplayFrames = 0;

    if (!gCapturePcm || gCapture.capturedFrames == 0)
        return;

    u32 start = 0;
    u32 end = gCapture.capturedFrames;

    // Trim most leading/trailing silence, but keep a short margin.
    if (gCapture.firstActiveFrame < gCapture.capturedFrames) {
        const u32 margin = 320;

        start = gCapture.firstActiveFrame > margin
              ? gCapture.firstActiveFrame - margin
              : 0;

        end = gCapture.lastActiveFrame + margin + 1;
        if (end > gCapture.capturedFrames)
            end = gCapture.capturedFrames;
    }

    if (end <= start)
        return;

    gReplayFrames = end - start;
    gReplayMono = (s16 *)linearAlloc(gReplayFrames * sizeof(s16));
    if (!gReplayMono) {
        gReplayFrames = 0;
        return;
    }

    // The diagnostic tone is identical in L/R, so using the left channel
    // gives CSND a normal mono PCM16 stream.
    for (u32 i = 0; i < gReplayFrames; i++)
        gReplayMono[i] = gCapturePcm[(start + i) * 2];
}

static Result play_csnd_first_available(s16 *audio, u32 bytes, int *usedChannel)
{
    for (int ch = 0; ch < CSND_NUM_CHANNELS; ch++) {
        Result rc = csndPlaySound(
            ch,
            SOUND_FORMAT_16BIT | SOUND_ONE_SHOT | SOUND_LINEAR_INTERP,
            CAPTURE_RATE,
            1.0f,
            0.0f,
            audio,
            audio,
            bytes
        );

        // libctru returns 1 when the requested channel could not be acquired.
        if (rc != 1) {
            *usedChannel = ch;
            return rc;
        }
    }

    *usedChannel = -1;
    return 1;
}

static void run_direct_csnd_reference(void)
{
    const u32 frames = CAPTURE_RATE;
    const u32 bytes = frames * sizeof(s16);

    s16 *tone = (s16 *)linearAlloc(bytes);
    if (!tone)
        return;

    for (u32 i = 0; i < frames; i++) {
        double phase = (2.0 * M_PI * TONE_FREQ * (double)i) /
                       (double)CAPTURE_RATE;
        tone[i] = (s16)(sin(phase) * TONE_AMPLITUDE);
    }

    Result rc = csndInit();
    if (R_SUCCEEDED(rc)) {
        GSPGPU_FlushDataCache(tone, bytes);
        int ch = -1;
        play_csnd_first_available(tone, bytes, &ch);
        svcSleepThread(1200LL * 1000LL * 1000LL);
        csndExit();
    }

    linearFree(tone);
}

static void run_ndsp_capture(void)
{
    memset(&gCapture, 0, sizeof(gCapture));
    gCapture.attempted = true;
    gCapture.ndspRc = (Result)-1;

    if (gCapturePcm) {
        linearFree(gCapturePcm);
        gCapturePcm = NULL;
    }
    if (gReplayMono) {
        linearFree(gReplayMono);
        gReplayMono = NULL;
    }
    gReplayFrames = 0;
    memset(&gReplay, 0, sizeof(gReplay));

    u32 *input = (u32 *)linearAlloc(INPUT_FRAMES * sizeof(u32));
    gCapturePcm = (s16 *)linearAlloc(CAPTURE_FRAMES * 2 * sizeof(s16));

    if (!input || !gCapturePcm) {
        if (input) linearFree(input);
        if (gCapturePcm) {
            linearFree(gCapturePcm);
            gCapturePcm = NULL;
        }
        return;
    }

    memset(gCapturePcm, 0, CAPTURE_FRAMES * 2 * sizeof(s16));
    fill_input_tone(input);
    DSP_FlushDataCache(input, INPUT_FRAMES * sizeof(u32));

    gCapture.ndspRc = ndspInit();
    if (R_FAILED(gCapture.ndspRc)) {
        linearFree(input);
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);

    ndspWaveBuf capture;
    memset(&capture, 0, sizeof(capture));
    capture.data_pcm16 = gCapturePcm;
    capture.nsamples = CAPTURE_FRAMES;
    capture.offset = 0;

    ndspSetCapture(&capture);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, INPUT_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    ndspWaveBuf wave;
    memset(&wave, 0, sizeof(wave));
    wave.data_vaddr = input;
    wave.nsamples = INPUT_FRAMES;

    ndspChnWaveBufAdd(0, &wave);

    // Let the DSP produce almost the whole 2-second tone.
    svcSleepThread(100LL * 1000LL * 1000LL);
    gCapture.during = read_hw();

    // Total capture time 2.25 s: under the 3 s buffer, so no wrap expected.
    svcSleepThread(2150LL * 1000LL * 1000LL);

    ndspSetCapture(NULL);

    u32 frames = capture.offset;
    if (frames > CAPTURE_FRAMES)
        frames = CAPTURE_FRAMES;

    gCapture.droppedFrames = ndspGetDroppedFrames();

    ndspChnReset(0);
    ndspExit();
    linearFree(input);

    analyze_capture(frames);
    prepare_replay_buffer();
}

static void run_csnd_replay(void)
{
    memset(&gReplay, 0, sizeof(gReplay));
    gReplay.attempted = true;
    gReplay.channel = -1;
    gReplay.framesPlayed = gReplayFrames;
    gReplay.csndRc = (Result)-1;
    gReplay.playRc = (Result)-1;

    if (!gReplayMono || gReplayFrames == 0)
        return;

    gReplay.csndRc = csndInit();
    if (R_FAILED(gReplay.csndRc))
        return;

    u32 bytes = gReplayFrames * sizeof(s16);
    GSPGPU_FlushDataCache(gReplayMono, bytes);

    gReplay.playRc = play_csnd_first_available(
        gReplayMono, bytes, &gReplay.channel);

    svcSleepThread(50LL * 1000LL * 1000LL);
    gReplay.during = read_hw();

    u64 ms = ((u64)gReplayFrames * 1000ULL) / (u64)CAPTURE_RATE;
    svcSleepThread((s64)(ms + 250ULL) * 1000LL * 1000LL);

    csndExit();
}

static void write_u16_le(FILE *f, u16 v)
{
    u8 b[2] = { (u8)(v & 0xFF), (u8)((v >> 8) & 0xFF) };
    fwrite(b, 1, 2, f);
}

static void write_u32_le(FILE *f, u32 v)
{
    u8 b[4] = {
        (u8)(v & 0xFF),
        (u8)((v >> 8) & 0xFF),
        (u8)((v >> 16) & 0xFF),
        (u8)((v >> 24) & 0xFF)
    };
    fwrite(b, 1, 4, f);
}

static bool save_capture_wav(void)
{
    if (!gCapturePcm || gCapture.capturedFrames == 0)
        return false;

    FILE *f = fopen("sdmc:/ndsp_capture.wav", "wb");
    if (!f)
        return false;

    u32 dataBytes = gCapture.capturedFrames * 2 * sizeof(s16);
    u32 byteRate = CAPTURE_RATE * 2 * sizeof(s16);
    u16 blockAlign = 2 * sizeof(s16);

    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + dataBytes);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);
    write_u16_le(f, 1);            // PCM
    write_u16_le(f, 2);            // stereo
    write_u32_le(f, CAPTURE_RATE);
    write_u32_le(f, byteRate);
    write_u16_le(f, blockAlign);
    write_u16_le(f, 16);

    fwrite("data", 1, 4, f);
    write_u32_le(f, dataBytes);
    fwrite(gCapturePcm, 1, dataBytes, f);

    fclose(f);
    return true;
}

static bool save_report(void)
{
    FILE *f = fopen("sdmc:/3ds_audio_i2s_diag.txt", "w");
    if (!f)
        return false;

    fprintf(f, "3DS Audio I2S Diagnostic v0.6\n");
    fprintf(f, "NDSP final-mix capture -> CSND bridge proof\n");
    fprintf(f, "INPUT_TONE=%dHz input_rate=%d input_seconds=%d\n",
            (int)TONE_FREQ, INPUT_RATE, INPUT_SECONDS);
    fprintf(f, "NDSP_NATIVE_CAPTURE_RATE=%lu\n",
            (unsigned long)CAPTURE_RATE);

    fprintf(f, "\n=== NDSP_CAPTURE ===\n");
    fprintf(f, "ATTEMPTED=%u\n", gCapture.attempted ? 1 : 0);
    fprintf(f, "NDSP_INIT rc=0x%08lX (%s)\n",
            (unsigned long)gCapture.ndspRc,
            R_SUCCEEDED(gCapture.ndspRc) ? "OK" : "FAIL");
    fprintf(f, "CAPTURED_FRAMES=%lu\n",
            (unsigned long)gCapture.capturedFrames);
    fprintf(f, "CAPTURED_SECONDS=%.6f\n",
            (double)gCapture.capturedFrames / (double)CAPTURE_RATE);
    fprintf(f, "FIRST_ACTIVE_FRAME=%lu\n",
            (unsigned long)gCapture.firstActiveFrame);
    fprintf(f, "LAST_ACTIVE_FRAME=%lu\n",
            (unsigned long)gCapture.lastActiveFrame);
    fprintf(f, "NONZERO_ACTIVE_FRAMES=%lu\n",
            (unsigned long)gCapture.nonzeroFrames);
    fprintf(f, "PEAK_L=%d PEAK_R=%d\n",
            gCapture.peakL, gCapture.peakR);
    fprintf(f, "RMS_L=%.3f RMS_R=%.3f\n",
            gCapture.rmsL, gCapture.rmsR);
    fprintf(f, "ESTIMATED_CAPTURE_FREQ_HZ=%.3f\n",
            gCapture.estimatedHz);
    fprintf(f, "DROPPED_FRAMES=%lu\n",
            (unsigned long)gCapture.droppedFrames);
    fprintf(f, "HW_DURING_NDSP PDN_DSP=0x%02X I2S1_CNT=0x%04X I2S2_CNT=0x%04X\n",
            gCapture.during.pdnDsp,
            gCapture.during.i2s1,
            gCapture.during.i2s2);

    fprintf(f, "\n=== CSND_REPLAY_OF_DSP_CAPTURE ===\n");
    fprintf(f, "ATTEMPTED=%u\n", gReplay.attempted ? 1 : 0);
    fprintf(f, "CSND_INIT rc=0x%08lX (%s)\n",
            (unsigned long)gReplay.csndRc,
            R_SUCCEEDED(gReplay.csndRc) ? "OK" : "FAIL");
    fprintf(f, "CSND_PLAY rc=0x%08lX (%s)\n",
            (unsigned long)gReplay.playRc,
            R_SUCCEEDED(gReplay.playRc) ? "OK" : "FAIL");
    fprintf(f, "CSND_CHANNEL=%d\n", gReplay.channel);
    fprintf(f, "REPLAY_FRAMES=%lu\n",
            (unsigned long)gReplay.framesPlayed);
    fprintf(f, "HW_DURING_CSND PDN_DSP=0x%02X I2S1_CNT=0x%04X I2S2_CNT=0x%04X\n",
            gReplay.during.pdnDsp,
            gReplay.during.i2s1,
            gReplay.during.i2s2);

    bool capturedSignal =
        gCapture.capturedFrames > 0 &&
        gCapture.peakL > 128 &&
        gCapture.nonzeroFrames > 1000;

    fprintf(f, "\n=== INTERPRETATION ===\n");
    fprintf(f, "DSP_FINAL_PCM_CAPTURE_HAS_SIGNAL=%u\n",
            capturedSignal ? 1 : 0);
    fprintf(f, "EXPECTED_TEST_TONE_HZ=440\n");
    fprintf(f, "If Y is audible and ESTIMATED_CAPTURE_FREQ_HZ is near 440,\n");
    fprintf(f, "the DSP final mix is valid and replayable through CSND/I2S2.\n");

    fclose(f);
    return true;
}

static void draw(void)
{
    printf("\x1b[2J\x1b[H");
    printf("3DS Audio I2S Diagnostic v0.6\n");
    printf("DSP final PCM -> CSND bridge test\n\n");

    printf("[A] Direct CSND 440Hz reference\n");
    printf("    (should be audible on your console)\n\n");

    printf("[X] Generate tone in NDSP + CAPTURE FINAL MIX\n");
    if (gCapture.attempted) {
        printf("    ndsp: %s  captured: %lu frames\n",
               R_SUCCEEDED(gCapture.ndspRc) ? "OK" : "FAIL",
               (unsigned long)gCapture.capturedFrames);
        printf("    peak L/R: %d / %d\n",
               gCapture.peakL, gCapture.peakR);
        printf("    RMS L/R: %.1f / %.1f\n",
               gCapture.rmsL, gCapture.rmsR);
        printf("    estimated: %.1f Hz\n",
               gCapture.estimatedHz);
    }

    printf("\n[Y] Replay CAPTURED DSP PCM through CSND\n");
    if (gReplay.attempted) {
        printf("    csnd: %s  play: %s  ch:%d\n",
               R_SUCCEEDED(gReplay.csndRc) ? "OK" : "FAIL",
               R_SUCCEEDED(gReplay.playRc) ? "OK" : "FAIL",
               gReplay.channel);
    }

    printf("\n[B] Save log + ndsp_capture.wav\n");
    printf("[START] Exit\n\n");

    printf("Sequence: A -> X -> Y -> B\n");
    printf("Most important: tell me if Y is audible.\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    draw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START)
            break;

        if (down & KEY_A) {
            run_direct_csnd_reference();
            draw();
        }

        if (down & KEY_X) {
            run_ndsp_capture();
            draw();
        }

        if (down & KEY_Y) {
            run_csnd_replay();
            draw();
        }

        if (down & KEY_B) {
            bool logOk = save_report();
            bool wavOk = save_capture_wav();
            draw();
            printf("\nlog: %s\n", logOk ? "saved" : "FAILED");
            printf("wav: %s\n", wavOk ? "saved" : "FAILED");
            printf("sd:/3ds_audio_i2s_diag.txt\n");
            printf("sd:/ndsp_capture.wav\n");
        }

        gspWaitForVBlank();
    }

    if (gReplayMono)
        linearFree(gReplayMono);
    if (gCapturePcm)
        linearFree(gCapturePcm);

    gfxExit();
    return 0;
}
