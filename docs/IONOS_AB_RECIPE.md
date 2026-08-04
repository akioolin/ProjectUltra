# IONOS rig A/B — the exact recipe

Hand this whole file to the other AI. It is everything needed to run and analyse a
throughput A/B on the live IONOS rig the way it has been run in this project.

---

## 0. MANDATORY — operate under this stack for every judgement below

Every technical answer, design decision and analysis must be written from the *combined and
mandatory* standpoint of:

1. **PHY theorist** (primary) — PhD-level HF modem researcher: channel coding, OFDM/MC-DPSK
   theory, ARQ, channel estimation under fading, calibrated LLRs under a documented noise
   model, per-carrier SNR with documented reference, explicit channel-reciprocity
   assumptions, information-theoretic limits.
2. **Real-time DSP systems engineer** (mandatory secondary) — fixed/floating numerics,
   FFT/PLL/AGC/equalizer pipelines, buffer management, multi-threaded audio paths,
   lifecycle/state-machine correctness, resource cleanup across session boundaries.
3. **Veteran HF operator** (mandatory tertiary) — ALC/audio gain staging on real radios,
   antenna mismatch, QSB/QRM, multipath QSO behaviour, tolerable vs unacceptable failures in
   a shift, defaults/UX at 2 AM in a noisy shack.
4. **First-principles physics escape hatch** — when the three disagree, fall back to
   physics/information theory and let the model arbitrate.

Reject heuristic / "tweak the threshold" patches that lack a principled justification under
all three mandatory lenses.

**Hard physical constraints — check every claim against these:** half-duplex (one frequency,
no TX+RX at once; the ACK gap is the other station's turn, not reclaimable sender airtime);
PA duty cycle (no ~100% key-down); the information-theoretic ceiling (modulation_bits × baud
× code_rate × data_carriers); stale CSI (Doppler sets coherence time, delay spread sets
coherence bandwidth — you always equalise with a past estimate); fading loss is irreducible
(deep nulls destroy frames regardless of estimator quality, so "zero retx on fading" is
unphysical); no shared timebase between the two rigs.

---

## 1. Rig topology

| | |
|---|---|
| **Mac** (this machine) | receiver / call acceptor. Holds RX rate authority. |
| **Pi 5** | sender. `ssh -n -o ConnectTimeout=10 -o BatchMode=yes -i ~/.ssh/id_pi5 math@192.168.160.163` |
| Path | Pi5 → IONOS HF channel simulator → Mac (real audio, real soundcards) |
| Test file | `/home/math/testfile_50k.bin` on the Pi5 (50 KB) |
| Channel | set on the IONOS box itself — MPG = ITU Good (0.1 Hz / 0.5 ms), MPM = Moderate |

The Pi5 alias `pi5` does NOT resolve — use the IP. `logs/gui.log` is the real trace on both
ends, not stdout.

---

## 2. Run one A/B (copy this shape exactly)

```bash
SSH="ssh -n -o ConnectTimeout=10 -o BatchMode=yes -i ~/.ssh/id_pi5 math@192.168.160.163"
cd ~/Projects/ProjectUltra
kill_both(){ pkill -9 -f 'ultra_gui' 2>/dev/null; $SSH "pkill -9 -f '[u]ltra_gui'" >/dev/null 2>&1; }

for pair in 1 2 3 4 5 6 7 8; do
  for arm in TEST BASE; do
    [ "$arm" = TEST ] && menv="ULTRA_YOUR_KNOB=1" || menv=""
    kill_both; sleep 4
    : > logs/gui.log
    $SSH "cd ~/ProjectUltra && : > logs/gui.log" >/dev/null 2>&1
    rm -f ~/Downloads/testfile_50k.bin

    # The caller owns teardown. Do not arm completion-driven DISCONNECT on the responder:
    # the final file ACK completes both sides at once and would otherwise cross the closes.
    env $menv nohup ./build/ultra_gui --auto-accept --log-level info --log-category all \
        --exit-after 400 > /tmp/run.out 2>&1 &
    sleep 5
    $SSH "cd ~/ProjectUltra && DISPLAY=:0 XAUTHORITY=/home/math/.Xauthority \
          setsid nohup ./build/ultra_gui --auto-connect MAC --connect-delay 8 \
          --auto-send-file /home/math/testfile_50k.bin --auto-disconnect-after 350 \
          --disconnect-on-file-done --exit-after 400 --log-level info --log-category all \
          </dev/null >/tmp/run_pi5.out 2>&1 & exit 0" >/dev/null 2>&1

    # confirm the sender actually started, else the pair is void
    up=0; for t in 1 2 3 4 5 6; do sleep 5
      [ "$($SSH 'pgrep -c "[u]ltra_gui" || echo 0' | tr -d '[:space:]')" != "0" ] && { up=1; break; }; done
    [ "$up" != 1 ] && { echo "pair $pair [$arm]: SENDER FAILED"; kill_both; continue; }

    w=0; line=""
    while [ $w -lt 380 ]; do
      line=$(grep -hE "\[FILE\] Received" logs/gui.log | tail -1)
      [ -n "$line" ] && break; sleep 5; w=$((w+5))
    done
    sleep 6
    cp logs/gui.log /tmp/ab_${pair}_${arm}.log
    scp -q -i ~/.ssh/id_pi5 math@192.168.160.163:~/ProjectUltra/logs/gui.log \
        /tmp/ab_${pair}_${arm}_pi5.log 2>/dev/null
    kill_both
    kbps=$(echo "$line" | grep -oE '[0-9.]+ kbps' | grep -oE '[0-9.]+')
    echo "pair $pair [$arm]: ${kbps:-TIMEOUT} kbps"
  done
done
```

**Run it with `nohup ... &`** — 8 pairs is ~1.6 h and the shell will time out otherwise.

---

## 3. Non-negotiable rules for the design

- **INTERLEAVE the arms.** Never run all of arm A then all of arm B. The channel drifts;
  cross-epoch comparison has read −10% on a 23%-rougher epoch. The statistic is the
  **per-pair delta**, not the arm means.
- **n ≥ 8 pairs.** Paired sd on this rig is **14–36%**, so n=8 resolves ~15% and **n=3
  resolves nothing**. Four claims were retracted in one campaign for believing n=2–3.
- **Exclude timeouts from the goodput means, count them separately.** A deadlocked run scores
  ~0 and swamps the comparison it is not evidence about.
- **Ship a NULL CONTROL.** Grep the log for proof the knob engaged (a log line it emits, a
  counter, a config dump). A knob that provably did not engage proves nothing — this has
  happened repeatedly here.
- **Pre-commit a FALSIFIER** before running: "if X drops but goodput does not, the hypothesis
  is wrong." Write it down first.
- **The seeded sim gate cannot A/B a rate controller.** Different decisions shift the
  timeline, so each burst meets a different point in the fading realisation even at a fixed
  seed. Use `tools/gui_qso_scenario.sh` for PASS/FAIL only.

---

## 4. Analysis

```python
import statistics, math
d = [ ... per-pair percentage deltas ... ]          # (test-base)/base * 100
n = len(d); m = statistics.mean(d); sd = statistics.stdev(d); se = sd/math.sqrt(n)
pos = sum(1 for x in d if x > 0)
# sign test (assumes nothing about the distribution)
k = max(pos, n-pos)
p_sign = 2*sum(math.comb(n,i) for i in range(k, n+1)) / 2**n
# paired t (uses magnitudes; more powerful when differences are ~symmetric)
t = m/se
```

**Report BOTH tests and say if they disagree.** On the one significant result in this project
they did: t p=0.022 vs sign p=0.070. Quoting only the favourable one is not acceptable.

Also report the **mechanism metric**, not just goodput — mode changes/run, craters, ACK
defers, whatever the knob is supposed to move. Goodput alone called four different levers a
"wash" when the mechanism metrics decided every one of them.

---

## 5. Traps that have actually bitten, in this exact setup

1. **Cross-side attribution.** Reading a number from the receiver's log and asserting it
   about the sender. Cost three retractions. The two ends have *no shared timebase*; the Mac
   is NOT the file sender.
2. **`grep -c ... || echo 0` emits `0\n0`** when there are no matches (grep exits 1 *and*
   prints 0), which corrupts CSV rows. Parse the runner's stdout instead.
3. **Stale objects after `cmake --build --target X`.** Adding a member to a header and then
   building selective targets leaves other TUs on the old layout — symptoms look like a
   logic bug. Do a full build when a header's layout changes.
4. **`UltraTncSimAudio` fails under CPU load** and passes standalone (~57 s). Do not chase it
   while a rig A/B or a workflow is running.
5. **Older builds deadlocked the handshake ~1-in-5**
   (BUG-CONNECT-ACK-RESCUE-DISARM): a sync correlation was wrongly treated as proof the peer
   decoded CONNECT_ACK. The 2026-08-03 cache-only/reactive cross-mode fix has deterministic
   coverage but still needs a fresh IONOS transfer. Any pre-fix deadlock run remains void,
   not evidence.
6. **Levels.** A low Pi5 PipeWire sink volume looks exactly like a dead link (peer rms flat
   ~0.03, 0 chirp locks). `wpctl set-volume 57 1.0`.
7. **Long-profile flags are endpoint policy, not negotiation.**
   `ULTRA_8PSK_LONG_LDPC` and `ULTRA_QPSK_R34_LONG_LDPC` must be identical on Mac and
   Pi. The immutable runner writes and checks both endpoint env manifests; an ad-hoc run
   that does not prove parity is invalid. The selector can price the configured CW/Z
   geometry, but its FER model is still logical-rung/Z27 calibrated, so do not use an
   adaptive long-profile run as a graduation test.
8. **ARQ progress is not physical k/N.** Cumulative base retirement can legitimately
   report `9/8`, `11/6`, or more after an old base hole releases already-SACKed suffixes.
   Any physical-round optimization must bind the exact serialized frame identities before
   and after the accepted ACK; never infer the latest burst outcome from cumulative ARQ
   progress.

---

## 6. Current state (2026-08-04, v0.5.1-pre-alpha)

- `ULTRA_LATENT_RATE` is **DEFAULT-ON** — outcome-fitted latent-state rate controller,
  +14.5% (8 pairs, p=0.022). `=0` restores the legacy SNR-anchor ladder.
- Fresh fixed-build reference transfers are byte-exact and teardown-clean: QPSK R3/4
  cw3/Z81 delivered **1.890 kbps physical / 2.143 kbps keyed**; 8PSK R2/3 cw4/Z81
  delivered **1.979 / 2.260 kbps**. A favorable independent 8PSK draw reached
  2.494 / 2.862 kbps. These are sequential realizations, not a powered A/B; QPSK is
  the steadier fallback and 8PSK is the higher but more variable rung.
- The descriptor-only partial-repair A/B is complete and is a **MEASURED WASH**.
  Pair 1 is void as a whole because OFF recorded a Pi RX FIFO overrun; pairs 2--9
  provide eight valid order-balanced pairs. Physical paired effect was **+1.468%**,
  95% CI `[-14.006,+16.942]%`, `p=0.8289`; sign 5/8, `p=0.7266`; log effect
  `-0.119%`. Sixty-one engagements mechanically avoided 73.2 s (3.975% mean enabled
  physical span), but ON saw 12 craters versus 4 OFF and two enabled runs lost an
  engaged light repair after severe partials. Keep strict default-OFF and redesign
  acquisition diversity before any new campaign. The observed log SD 0.194 means
  n=8 cannot resolve the expected ~4% effect.
- A clean steady N8 8PSK cycle is about **3.02 kbps**, but a complete 50 KiB transfer
  also pays FILE_START, a short final group, eleven descriptors/guards, and ten measured
  half-duplex turnarounds. Even with zero frame loss the current format is only about
  **2.95–2.96 kbps physical-span goodput**. Therefore 3,000 bps for the complete file
  needs both near-zero repairs and a small structural overhead reduction; ACK tuning
  alone cannot close a loss-heavy run because retransmitted key-down is the dominant cost.
- Full default-off knob inventory: `docs/ENV_KNOBS_DEFAULT_OFF.md`.
