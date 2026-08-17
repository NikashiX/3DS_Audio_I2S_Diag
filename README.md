# 3DS Audio I2S Diagnostic

Diagnostic homebrew for **Old Nintendo 3DS / CTR**. It is designed for the specific case where **CSND/I2S2 audio works but NDSP/DSP audio is silent**.

## Safety

Version 0.1 is intentionally **read-only** with respect to PDN/CODEC hardware registers. It does not write clocks, resets, routing or CODEC configuration.

It reads:

- `PDN_I2S_CNT` physical `0x10141220` -> process virtual `0x1EC41220`
- `PDN_DSP_CNT` physical `0x10141230` -> process virtual `0x1EC41230`

The CIA RSF grants access only to the PDN IO page plus the standard DSP memory mapping needed for NDSP.

## On-console controls

- **A**: initialize CSND, take a PDN snapshot, then exit CSND.
- **X**: initialize `dsp::DSP`, take a snapshot, query headphone status, then exit.
- **Y**: initialize NDSP and take a snapshot **while NDSP is active**. This is the key test.
- **B**: save `sdmc:/3ds_audio_i2s_diag.txt`.
- **START**: exit.

## What to look for

During the **Y / NDSP** test, `PDN_DSP_CNT` is the key value:

- bit 0 = DSP out of reset when set
- bit 1 = DSP clock enabled when set

If `ndspInit()` succeeds but either of these is 0 while NDSP is active, that is suspicious and gives us a concrete software/power-state lead.

`PDN_I2S_CNT` bit 1 is the I2S2 clock. 3dbrew documents bit 0 as an uncertain/possibly unimplemented I2S1 clock bit, so do not over-interpret bit 0 by itself.

## Build

Requires current devkitPro/devkitARM + `3ds-dev`, plus `makerom` and `bannertool` in PATH.

```sh
make
make cia
```

Outputs:

- `audio_i2s_diag.3dsx`
- `audio_i2s_diag.cia`

## Report

After running A, X and Y, press B. Send the contents of:

`sdmc:/3ds_audio_i2s_diag.txt`

Do not attempt to manually change PDN registers from another tool yet. A later test build can conditionally toggle/reset the DSP only after the read-only results are known.
