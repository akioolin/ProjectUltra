# Waveform Selection Test Data

**Date:** 2026-01-29
**Test conditions:** CFO=20Hz, 5 frames per test, seed=42

## Fading Index by Channel Type

| Channel | MC-DPSK Fading | OFDM Fading | Description |
|---------|----------------|-------------|-------------|
| AWGN | 0.01-0.02 | 0.06-0.08 | No fading (flat channel) |
| Good | 0.24-0.31 | 0.36 | Light multipath fading |
| Moderate | 0.32-0.43 | 0.44-0.46 | Significant fading |
| Poor | 0.26-0.30 | - | Heavy fading |

**Fading Threshold:** > 0.4 indicates significant fading (used in `isFading()`)

**Note:** Fading detection now implemented for both MC-DPSK and OFDM waveforms using per-carrier magnitude coefficient of variation.

## MC-DPSK Performance (8 carriers, R1/4)

| Channel | SNR 0 | SNR 5 | SNR 10 |
|---------|-------|-------|--------|
| AWGN | 80% | 100% | 100% |
| Good | 80% | 100% | 100% |
| Moderate | 80% | 100% | 100% |
| Poor | - | 100% | 100% |

**Throughput:** ~938 bps
**CFO tolerance:** ±50 Hz (verified)
**Recommended range:** SNR 0-10 dB

## OFDM_CHIRP Performance (SNR=10 dB, CFO=20Hz, via StreamingDecoder)

| Rate | AWGN (fading=0.07) | Good (fading=0.36) | Moderate (fading=0.46) |
|------|--------------------|--------------------|------------------------|
| R1/4 | 100% | 80% | 80% |
| R1/2 | 80% | 40% | 20% |
| R2/3 | 80% | 40% | 0% |

**Throughput by rate:**
- R1/4: ~850 bps
- R1/2: ~1.7 kbps
- R2/3: ~2.3 kbps

## Key Findings

1. **Fading index determines code rate capability (not SNR)**
   - fading < 0.1 (AWGN): Higher rates work (R1/2, R2/3)
   - fading 0.3-0.4 (good): R1/2 degrades to 40%, use R1/4
   - fading > 0.4 (moderate): Only R1/4 works, R2/3 fails completely

2. **Fading detection now works for both MC-DPSK and OFDM**
   - Uses coefficient of variation of per-carrier magnitudes
   - Threshold 0.4 separates "mild" from "significant" fading
   - Can be used for dynamic rate selection

3. **MC-DPSK is faster than OFDM_CHIRP R1/4 at same reliability**
   - MC-DPSK 8 carriers: ~938 bps
   - OFDM_CHIRP R1/4: ~850 bps
   - No benefit to using OFDM_CHIRP below 10 dB

4. **Rate selection by fading index:**
   - fading < 0.1: Use R2/3 or higher (~2.3+ kbps)
   - fading 0.1-0.35: Use R1/2 (~1.7 kbps)
   - fading > 0.35: Use R1/4 or stay on MC-DPSK

## Recommended Waveform Selection Algorithm (FINAL)

```
if (snr < 10 dB):
    use MC-DPSK 8 carriers (R1/4)  # ~938 bps, most robust

elif (fading < 0.1):  # AWGN-like (no fading)
    if (snr >= 17 dB):
        use OFDM_COX R2/3  # ~5.3 kbps (needs testing)
    else:
        use OFDM_CHIRP R2/3  # ~2.3 kbps

else:  # Any fading (>= 0.1)
    use MC-DPSK 8 carriers (R1/4)  # ~938 bps
```

**Key insight:** OFDM only works reliably on AWGN-like channels (fading < 0.1). Any multipath fading (good/moderate/poor HF) degrades OFDM significantly - MC-DPSK is more reliable and often faster than OFDM with low code rates.

**Verified results (SNR=10, CFO=20Hz):**
| Channel | Fading | Algorithm Choice | Decode Rate |
|---------|--------|------------------|-------------|
| AWGN | 0.02 | OFDM_CHIRP R2/3 | 100% |
| Good | 0.32 | MC-DPSK R1/4 | 100% |
| Moderate | 0.43 | MC-DPSK R1/4 | 100% |

## TODO

1. ~~Implement fading detection for OFDM waveforms~~ DONE
2. Test OFDM_COX at 17+ dB with CFO and fading detection
3. Consider adaptive interleaver depth for fading channels
4. Wire fading-based rate selection into protocol negotiation
