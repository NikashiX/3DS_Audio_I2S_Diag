#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Physical PDN registers documented at 0x10141220 / 0x10141230.
// In a 3DS user process, mapped IO uses +0x0EB00000 virtual offset.
#define PDN_I2S_CNT_ADDR  ((uintptr_t)0x1EC41220)
#define PDN_DSP_CNT_ADDR  ((uintptr_t)0x1EC41230)

#define REG8_RO(addr) (*(volatile const u8 *)(addr))

typedef struct {
    u8 i2s;
    u8 dsp;
} PdnSnapshot;

typedef struct {
    bool valid;
    char name[32];
    Result rc;
    PdnSnapshot during;
    PdnSnapshot after;
    bool hpValid;
    bool headphones;
    Result hpRc;
} TestResult;

static PdnSnapshot startup;
static TestResult csndTest;
static TestResult dspTest;
static TestResult ndspTest;

static PdnSnapshot read_pdn(void)
{
    __asm__ volatile("dmb" ::: "memory");
    PdnSnapshot s;
    s.i2s = REG8_RO(PDN_I2S_CNT_ADDR);
    s.dsp = REG8_RO(PDN_DSP_CNT_ADDR);
    __asm__ volatile("dmb" ::: "memory");
    return s;
}

static const char *yn(bool v) { return v ? "ON" : "OFF"; }

static void print_snapshot(const char *label, PdnSnapshot s)
{
    printf("%s\n", label);
    printf("  PDN_I2S_CNT: 0x%02X\n", s.i2s);
    printf("    bit0 I2S1 clock*: %s\n", yn((s.i2s & BIT(0)) != 0));
    printf("    bit1 I2S2 clock : %s\n", yn((s.i2s & BIT(1)) != 0));
    printf("  PDN_DSP_CNT: 0x%02X\n", s.dsp);
    printf("    bit0 DSP out-reset: %s\n", yn((s.dsp & BIT(0)) != 0));
    printf("    bit1 DSP clock    : %s\n", yn((s.dsp & BIT(1)) != 0));
}

static void draw(void)
{
    printf("\x1b[2J\x1b[H");
    printf("3DS Audio I2S Diagnostic v0.1\n");
    printf("READ-ONLY: no PDN/CODEC writes\n\n");
    print_snapshot("STARTUP", startup);
    printf("\n");

    if (csndTest.valid) {
        printf("[A] CSND init rc: 0x%08lX %s\n", (unsigned long)csndTest.rc,
               R_SUCCEEDED(csndTest.rc) ? "OK" : "FAIL");
        print_snapshot("  during CSND", csndTest.during);
    } else {
        printf("[A] Test CSND / I2S2\n");
    }

    printf("\n");
    if (dspTest.valid) {
        printf("[X] DSP service rc: 0x%08lX %s\n", (unsigned long)dspTest.rc,
               R_SUCCEEDED(dspTest.rc) ? "OK" : "FAIL");
        if (dspTest.hpValid)
            printf("  Headphones via DSP: %s (rc 0x%08lX)\n",
                   dspTest.headphones ? "inserted" : "not inserted",
                   (unsigned long)dspTest.hpRc);
        print_snapshot("  during DSP service", dspTest.during);
    } else {
        printf("[X] Test dsp::DSP service\n");
    }

    printf("\n");
    if (ndspTest.valid) {
        printf("[Y] NDSP init rc: 0x%08lX %s\n", (unsigned long)ndspTest.rc,
               R_SUCCEEDED(ndspTest.rc) ? "OK" : "FAIL");
        print_snapshot("  DURING NDSP (most important)", ndspTest.during);
        if (R_SUCCEEDED(ndspTest.rc)) {
            bool outReset = (ndspTest.during.dsp & BIT(0)) != 0;
            bool clockOn  = (ndspTest.during.dsp & BIT(1)) != 0;
            printf("  DSP power-state check: %s\n",
                   (outReset && clockOn) ? "clock+reset look enabled" : "SUSPICIOUS");
        }
    } else {
        printf("[Y] Test NDSP (IMPORTANT)\n");
    }

    printf("\n[B] Save report to SD\n");
    printf("[START] Exit\n");
    printf("\n* 3dbrew marks I2S1 bit0 as uncertain/unimplemented.\n");
}

static void run_csnd_test(void)
{
    memset(&csndTest, 0, sizeof(csndTest));
    csndTest.valid = true;
    strcpy(csndTest.name, "CSND");
    csndTest.rc = csndInit();
    csndTest.during = read_pdn();
    if (R_SUCCEEDED(csndTest.rc)) csndExit();
    svcSleepThread(20 * 1000 * 1000LL);
    csndTest.after = read_pdn();
}

static void run_dsp_test(void)
{
    memset(&dspTest, 0, sizeof(dspTest));
    dspTest.valid = true;
    strcpy(dspTest.name, "DSP service");
    dspTest.rc = dspInit();
    dspTest.during = read_pdn();
    if (R_SUCCEEDED(dspTest.rc)) {
        bool hp = false;
        dspTest.hpRc = DSP_GetHeadphoneStatus(&hp);
        dspTest.hpValid = R_SUCCEEDED(dspTest.hpRc);
        dspTest.headphones = hp;
        dspExit();
    }
    svcSleepThread(20 * 1000 * 1000LL);
    dspTest.after = read_pdn();
}

static void run_ndsp_test(void)
{
    memset(&ndspTest, 0, sizeof(ndspTest));
    ndspTest.valid = true;
    strcpy(ndspTest.name, "NDSP");
    ndspTest.rc = ndspInit();
    // Snapshot while NDSP is initialized: this is the key state.
    ndspTest.during = read_pdn();
    if (R_SUCCEEDED(ndspTest.rc)) ndspExit();
    svcSleepThread(20 * 1000 * 1000LL);
    ndspTest.after = read_pdn();
}

static void fprint_snapshot(FILE *f, const char *label, PdnSnapshot s)
{
    fprintf(f, "%s\n", label);
    fprintf(f, "PDN_I2S_CNT=0x%02X I2S1_bit0=%u I2S2_bit1=%u\n",
            s.i2s, !!(s.i2s & BIT(0)), !!(s.i2s & BIT(1)));
    fprintf(f, "PDN_DSP_CNT=0x%02X DSP_out_reset_bit0=%u DSP_clock_bit1=%u\n",
            s.dsp, !!(s.dsp & BIT(0)), !!(s.dsp & BIT(1)));
}

static bool save_report(void)
{
    FILE *f = fopen("sdmc:/3ds_audio_i2s_diag.txt", "w");
    if (!f) return false;

    fprintf(f, "3DS Audio I2S Diagnostic v0.1\n");
    fprintf(f, "READ-ONLY diagnostic; no PDN/CODEC writes.\n\n");
    fprint_snapshot(f, "STARTUP", startup);

    if (csndTest.valid) {
        fprintf(f, "\nCSND rc=0x%08lX (%s)\n", (unsigned long)csndTest.rc,
                R_SUCCEEDED(csndTest.rc) ? "OK" : "FAIL");
        fprint_snapshot(f, "CSND_DURING", csndTest.during);
        fprint_snapshot(f, "CSND_AFTER", csndTest.after);
    }

    if (dspTest.valid) {
        fprintf(f, "\nDSP_SERVICE rc=0x%08lX (%s)\n", (unsigned long)dspTest.rc,
                R_SUCCEEDED(dspTest.rc) ? "OK" : "FAIL");
        if (dspTest.hpValid)
            fprintf(f, "DSP_HEADPHONE=%u rc=0x%08lX\n", dspTest.headphones,
                    (unsigned long)dspTest.hpRc);
        fprint_snapshot(f, "DSP_SERVICE_DURING", dspTest.during);
        fprint_snapshot(f, "DSP_SERVICE_AFTER", dspTest.after);
    }

    if (ndspTest.valid) {
        fprintf(f, "\nNDSP rc=0x%08lX (%s)\n", (unsigned long)ndspTest.rc,
                R_SUCCEEDED(ndspTest.rc) ? "OK" : "FAIL");
        fprint_snapshot(f, "NDSP_DURING", ndspTest.during);
        fprint_snapshot(f, "NDSP_AFTER", ndspTest.after);
        fprintf(f, "NDSP_DSP_OUT_RESET=%u\n", !!(ndspTest.during.dsp & BIT(0)));
        fprintf(f, "NDSP_DSP_CLOCK=%u\n", !!(ndspTest.during.dsp & BIT(1)));
    }

    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    startup = read_pdn();
    draw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START) break;
        if (down & KEY_A) { run_csnd_test(); draw(); }
        if (down & KEY_X) { run_dsp_test(); draw(); }
        if (down & KEY_Y) { run_ndsp_test(); draw(); }
        if (down & KEY_B) {
            bool ok = save_report();
            draw();
            printf("\nReport: %s\n", ok ? "sdmc:/3ds_audio_i2s_diag.txt" : "FAILED TO SAVE");
        }

        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
