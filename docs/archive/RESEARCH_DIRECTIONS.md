# Research Directions

**Purpose:** Long-term research goals for exceeding industry HF modem performance.

---

## Ultimate Goal

**Advance research into HF modem speed - transmit faster than any existing system.**

This is a research project to push the boundaries of HF data transmission. The goal is not just to match industry leaders (~8.5 kbps) but to exceed them through novel techniques.

---

## Current vs Target Performance

| Metric | Industry Leader | ProjectUltra Now | Target |
|--------|-----------------|------------------|--------|
| Max throughput (AWGN) | 8.5 kbps | 7.2 kbps | **10+ kbps** |
| Fading @ 15 dB | 922 bps (lvl 9) | **938 bps** (verified) | **1.2+ kbps** |
| Fading @ 0 dB | 270 bps (lvl 5) | **234 bps** (verified) | **300+ bps** |
| Low-SNR floor | ~0 dB | -11 dB | -15 dB |
| Doppler tolerance | Good | Good (MC-DPSK) | Excellent |

**Gap:** Cannot reach Level 10 (1203 bps) on fading - R1/2 fails, need coherent modulation with pilots.

---

## Most Promising Techniques

### 1. OTFS Modulation
Prior prototype removed from the production build after it underperformed OFDM_CHIRP on the tested fading profiles. Revisit only as a separate research branch with real DD-domain channel estimation/equalization.
- [arxiv.org/pdf/2302.14224](https://arxiv.org/pdf/2302.14224)

### 2. Spatially-Coupled LDPC
Threshold saturation gives capacity-approaching performance. SC-LDPC codes achieve 0.07 dB from Shannon limit on Rayleigh fading. Could replace current 802.11n LDPC.
- [itsoc.org SC-LDPC tutorial](https://www.itsoc.org/sites/default/files/2021-03/laurent.schmalen@kit.edu%20-%20ESIT_20_Schmalen.pdf)

### 3. Faster-than-Nyquist (FTN)
Mazo limit allows 25% more bits in same bandwidth with trellis decoding. τ=0.8 gives no BER degradation. Requires iterative ISI cancellation.
- [comsoc.org FTN overview](https://www.comsoc.org/publications/ctn/running-faster-nyquist-idea-whose-time-may-have-come)

### 4. Turbo Equalization
Iterative detection + decoding. "Turbo DPSK" achieves 4.65 dB gain on Rayleigh fading at 10^-3 BER. Could boost our DPSK mode significantly.
- [academia.edu Turbo DPSK](https://www.academia.edu/81703561/Turbo_DPSK_iterative_differential_PSK_demodulation_and_channel_decoding)

### 5. OFDM-IM (Index Modulation)
Transmit extra bits via subcarrier activation patterns. OFDM-SPM doubles spectral efficiency. Lower complexity than higher QAM.
- [arxiv.org/html/2501.15437v1](https://arxiv.org/html/2501.15437v1)

---

## Secondary Techniques

6. **Higher-order modulation**: 64QAM, 256QAM (requires excellent SNR, NVIS/local)
7. **Polar codes**: Capacity-achieving, but LDPC already close
8. **Machine learning channel estimation**: Neural networks for HF prediction
9. **MIL-STD-188-110D Appendix D**: Walsh-DSSS for extreme robustness (75-1200 bps over 3-48 kHz)
    - [rapidm.com 110D](https://www.rapidm.com/standard/mil-std-188-110d/)

---

## Key Insight from Literature

Advanced doubly-selective-channel waveforms can beat plain OFDM in literature, but only with complete channel estimation and detection chains. Prototype code should stay out of production until it has a maintained simulator/hardware test gate.

OTFS should be treated as future research, not production code.

---

## Literature to Review

- ITU-R F.1487 (HF channel models)
- MIL-STD-188-110D Appendix D (14 waveforms, Walsh-DSSS)
- IEEE papers on OTFS, SC-LDPC, turbo equalization, and OFDM-IM
- Chinese/Russian HF modem research (often published in native languages)

---

## Priority Experiments

1. **Turbo DPSK**: Add iterative decoding to existing DPSK - literature shows 4.65 dB gain
2. **FTN signaling**: Try τ=0.8 (Mazo limit) with trellis decoder
3. **SC-LDPC**: Replace 802.11n codes with spatially-coupled - 0.07 dB from capacity
4. **OTFS research branch**: Rebuild only with DD-domain MMSE/sparse equalization before comparing to OFDM again

---

## Industry Leader Reference (2750 Hz Tactical Mode)

| Level | Symbol Rate | Carriers | Modulation | Net Rate (bps) |
|-------|-------------|----------|------------|----------------|
| 1-4 | 23-94 | 20-40 | FSK | 18-175 |
| 5-10 | 94 | 3-13 | 4PSK | 270-1203 |
| 11-17 | 42 | 59 | 4PSK-32QAM | 2423-8489 |

**Key observations:**
- High-speed modes (11-17): 59 carriers at 42 baud (~1024 FFT at 48 kHz)
- Uses COHERENT modulation: 4PSK, 8PSK, 16QAM, 32QAM - NOT differential
- Must use pilots for channel estimation with coherent modulation on fading channels

---

## Removed Prototype

**OTFS**: Removed from production build. The old prototype used simplified TF/single-tap DD processing and was not release-quality.
