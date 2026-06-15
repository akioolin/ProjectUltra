# pi5tnc — Raspberry Pi 5 + Fe-Pi Audio (SGTL5000) setup

**Status:** prepared 2026-06-15 (hardware not yet assembled). Turnkey checklist to bring
up the new clean-card station. Ties to `docs/KNOWN_BUGS.md::BUG-IONOS-PI5-CHEAP-DAC` and
`docs/CHEAP_CARD_ROBUSTNESS_PLAN.md`.

> Items flagged **[VERIFY ON BOX]** are ones I could not confirm from the bench and you
> should sanity-check on the assembled hardware — no guessing. Everything else is
> confirmed against the kernel overlay / SGTL5000 ASoC driver and our hard-won gain-staging
> from the live IONOS bringup.

## Why this card fixes the blocker

The cheap USB dongle's TRANSMIT path measured **±7 Hz carrier jitter** (vs the SoundBlaster's
±0.5 Hz). ±7 Hz = ±53.8°/symbol differential rotation > DQPSK's ±45° decision margin → the
4-CW CONNECT/CONNECT_ACK decodes to garbage even at a clean, matched, non-clipping level.
That jitter comes from a USB-bus-derived sample clock.

The **Fe-Pi Audio** board is a dedicated NXP **SGTL5000** I2S codec with its **own audio
clock** (not derived from the USB bus). That is the *root-cause* fix for the jitter — it
puts the Pi5 in the SoundBlaster's clock class, where the modem is already proven correct in
sim. The codec reports to Linux as **`Fe-Pi Audio`** and its driver ships in the mainline
Raspberry Pi kernel.

(This card is the pragmatic "works now" path. The orthogonal *software* path to also make
the cheap dongle work — DBPSK control frames + per-carrier LLR weighting — is tracked
separately in `CHEAP_CARD_ROBUSTNESS_PLAN.md` and is NOT required for pi5tnc.)

## 0. Hardware assembly notes (from the ARSF "RPI Radio interface" board)

- Codec: NXP SGTL5000 → reports as **Fe-Pi Audio**.
- Two 3.5 mm jacks: **line-out** (→ feeds the IONOS / radio input) and **headphone**.
- 3-pin monitor connector: small ≥32 Ω speaker, **yellow = GND** (confirm pinout silk on board).
- PTT switch + RS232/TTL/CIV via MAX3221 (RS232 auto-detected). Not needed for the IONOS test.
- Two 50-pin extender sockets to raise the board over the Pi5 if it fouls anything.
- **[VERIFY ON BOX]** Fe-Pi was designed in the Pi3/4 era. The 40-pin I2S header is still
  exposed on the Pi5 (now behind the RP1 southbridge), so `dtoverlay=fe-pi-audio` should
  apply — but confirm the card actually enumerates after step 4 before assuming.

## 1. OS choice

**Recommended: Raspberry Pi OS Lite (64-bit, Bookworm).** Lowest friction for the Fe-Pi
overlay — the `fe-pi-audio` overlay and SGTL5000 driver are shipped and tested there, and
the audio stack is well-trodden for this exact board (ham-radio interface use). Headless,
no desktop.

> Ubuntu Server on Pi5 also uses `/boot/firmware/config.txt` with the same overlay
> mechanism, but the Fe-Pi overlay/driver packaging is less guaranteed out of the box. If
> you specifically want Ubuntu Server, fine — just confirm `/boot/firmware/overlays/`
> contains `fe-pi-audio.dtbo` (install `linux-modules-extra-raspi` if missing). For a first
> bringup, RPi OS Lite removes a variable.

## 2. Flash + headless first boot

Use Raspberry Pi Imager → "Raspberry Pi OS Lite (64-bit)". In the gear/edit settings:
- hostname: `pi5tnc`
- enable SSH (key or password)
- set username, Wi-Fi/locale as needed

Boot, then:
```bash
ssh <user>@pi5tnc.local
sudo apt update && sudo apt full-upgrade -y
sudo reboot
```

## 3. Build deps + deploy the branch + build

```bash
sudo apt install -y \
  build-essential cmake git pkg-config \
  libsdl2-dev libgl1-mesa-dev \
  alsa-utils \
  libhamlib-dev \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc
```
Notes:
- The GUI is **SDL2 + OpenGL + ImGui** (NOT Qt). SDL2 is also the real-hardware audio
  I/O path. The Vulkan/SDL3/glfw3 `find_package(... REQUIRED)` lines in the tree live only
  under `thirdparty/imgui/examples/` (never built) — ignore them.
- **hamlib is `REQUIRED` by default** (`ULTRA_USE_LIBHAMLIB=ON` → `pkg_check_modules(HAMLIB
  REQUIRED ...)`); without `libhamlib-dev` the configure step hard-fails. You don't need rig
  CAT for the IONOS test, so the alternative is `-DULTRA_USE_LIBHAMLIB=OFF`.
- **gRPC/protobuf:** `ULTRA_FETCH_GRPC=ON` (default) compiles gRPC *from source* (30–60+ min
  on a Pi5) if the system packages aren't found. The `libgrpc++ / protobuf*` packages let
  `find_package(... CONFIG)` skip that. The gRPC target is only the **simulator** — not
  needed for a real-hardware QSO — so if the system gRPC CMake config isn't picked up, prefer
  *not* building otasim over fetching gRPC. ✅ On Trixie/Pi5 the system packages above provide
  `/usr/lib/aarch64-linux-gnu/cmake/grpc/gRPCConfig.cmake`, so configure finds gRPC + protobuf
  via CMake (no source fetch) — confirmed.
Get the code with the clock+jitter + ratiometric work. The fix lives on branch
**`feat/mcdpsk-clock-jitter-tracking`** (commit `e66143e` = clock+jitter trackers; the
ratiometric CONNECT_ACK guard is also on this branch — confirm it's committed before you
rely on it):
```bash
git clone <repo-url-or-rsync-from-mac> ProjectUltra && cd ProjectUltra
git checkout feat/mcdpsk-clock-jitter-tracking
cmake -S . -B build && cmake --build build -j4
```
(Or `rsync` the Mac working tree over, same as last time.)

## 4. Enable the Fe-Pi Audio overlay  ✅ CONFIRMED WORKING (Pi5 + Debian Trixie, kernel 6.12)

Append to the END of `/boot/firmware/config.txt` (after the final `[all]`, so it applies to
the Pi5). Do NOT need `dtparam=audio=off` — the Fe-Pi enumerates as a separate card (card 2)
alongside the two HDMI audio devices:
```ini
# --- Fe-Pi Audio (SGTL5000) HAT ---
dtparam=i2c_arm=on
dtoverlay=fe-pi-audio
```
```bash
sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak   # back up first
# (append the lines above, e.g. with: sudo tee -a /boot/firmware/config.txt)
sudo reboot
```
**Pi5 note (the key worry, now resolved):** legacy I2S HAT overlays often fail on Pi5 because
the RP1 splits I2S into separate `i2s_clk_producer`/`i2s_clk_consumer` instances. The current
`fe-pi-audio.dtbo` is already Pi5-aware — its fixups bind **`i2s_clk_consumer`** (correct, since
the Fe-Pi's SGTL5000 is the clock master and the Pi is the slave/consumer), and the Pi5 base DT
exposes that symbol. The reported Trixie-vs-Bookworm I2S regression did **not** bite here. No
`dtparam=i2s=on` needed (the overlay self-enables the consumer instance); no `pcie-32bit-dma`
overlay (it would break RP1 I2S).

## 5. Verify the card enumerated

```bash
cat /proc/asound/cards          # expect a 3rd card: "2 [Audio] Fe-Pi_Audio - Fe-Pi Audio"
arecord -l                      # expect: card 2 ... Fe-Pi HiFi sgtl5000-0 (capture present)
dmesg | grep -i sgtl5000        # THE proof: "sgtl5000 1-000a: sgtl5000 revision 0x11"
```
The dmesg line (codec found on I2C bus 1 at addr 0x0a) is the definitive "board alive + driver
bound" check — you don't even need `i2c-tools` if it appears. If the card does NOT enumerate:
`i2cdetect -y 1` should show `0a` (board on the bus); if `0a` is absent it's HAT seating/power,
if `0a` is present but no card it's the I2S binding (Trixie regression — consider Bookworm).

## 6. ALSA mixer — route + initial levels  ✅ CONFIRMED control names (card 2)

**Two SGTL5000 defaults are WRONG for a radio interface and must be flipped:** capture mux
defaults to `MIC_IN` (want `LINE_IN`), and `Lineout` defaults to **muted** (the line-out is the
TX jack to the IONOS). Applied + verified:
```bash
C=2                                              # Fe-Pi card index (HDMI = 0,1)
amixer -c $C sset 'Capture Mux' LINE_IN          # RX source = line-in (was MIC_IN)
amixer -c $C sset 'Capture' 60%                  # ADC PGA gain 0-15 (set 9; tune in step 8)
amixer -c $C sset 'Capture Attenuate Switch (-6dB)' off
amixer -c $C sset 'Lineout' 60% unmute           # TX analog out 0-31 (was OFF/muted)
amixer -c $C sset 'PCM' 85% unmute               # DAC digital volume 0-192
amixer -c $C sset 'Headphone' 50% unmute         # 3-pin monitor speaker (optional)
sudo alsactl store                               # persist across reboots (needs sudo)
```
`alsamixer -c 2` for a visual check (F4 = capture, F3 = playback). The line-out jack feeds the
IONOS input; the headphone jack drives the 3-pin monitor speaker.

## 7. Point the modem at the Fe-Pi card  **[VERIFY ON BOX]**

Select **Fe-Pi Audio** as the modem's audio in/out — the same way you selected the USB
dongle on the old Pi5 (device picker / config / ALSA device string like `plughw:<C>,0`).
Use `plughw:` (format-converting) not raw `hw:` — the SGTL5000 advertises S16/S32, and the
modem feeds FLOAT, so `plughw` lets ALSA convert (matches what we did on the Mac).

## 8. Gain staging into the IONOS (hard-won — mirror the live bringup)

Goal: clean, non-clipping multi-carrier into the IONOS, and an RX level the decoder likes.

- **TX (Pi5 → IONOS input):** two knobs — the modem's `tx_drive` (digital peak, clamps
  [0.05, 0.7]) **and** the Fe-Pi analog `Lineout` volume. With a *clean* card, `tx_drive`
  should respond linearly again (unlike the cheap dongle, where the analog saturated and
  `tx_drive` barely moved the level — that was a hardware trait, not a software bug).
- **At the IONOS panel:** target the **average** `Lvl` *well under 1800 mvp-p*. Watch the
  crest-factor meter: a clean multi-carrier signal reads **CF ≈ 10 dB**. If you see
  **CF ≈ 1.01 dB (≈0.05 dB)** you are HARD-CLIPPING the input into a square wave — back off
  `Lineout`/`tx_drive` until CF climbs back to ~10 dB. CF is the fastest clipping diagnostic.
- IONOS CH-IN factory default = 1; we ran ~5 on the bench. Raise a station's RX with that
  station's **CH-OUT** (after the channel), not CH-IN (before), so you don't re-clip the input.
- **RX (IONOS output → Pi5 line-in):** tune `Capture` (step 6) so the modem sees roughly
  **0.16–0.20 in-band RMS** — the level at which the Mac decoded cleanly on the bench.

Channel mapping from the bench: Mac = CH2, Pi5 = CH1 (Pi5 TX→CH1 IN, Pi5 RX←CH1 OUT;
Mac←CH2 OUT). IONOS = WGN, start S:N = 20 dB.

## 9. QSO test — same as the sim

With both stations live on the IONOS, run the handshake → file transfer you ran in sim:
PING/PONG → CONNECT/CONNECT_ACK → MODE_CHANGE → file transfer → DISCONNECT.
**Callsigns must match the connect target** (e.g. Pi5 callsign = `PI5`, and the initiator
must target `PI5`) — a callsign mismatch silently drops CONNECT_ACK.

**Success criterion:** CONNECT_ACK now decodes (the jitter that killed it on the cheap
dongle is gone with the clean codec clock), handshake completes, file transfers CRC-clean —
i.e. the sim result, on real hardware.

## If CONNECT_ACK still fails on the clean card

Then the limit was NOT (only) the cheap-card jitter and we re-open the analysis — but the
strong prior (sim-proven modem + clean codec clock + matched non-clipping levels) is that it
works. Capture the decoder debug (`got NNNN soft bits`, CW pass/fail) and the IONOS CF/Lvl,
same diagnostics as the bench, before changing anything.

## References
- Overlay: `dtoverlay=fe-pi-audio` (mainline RPi kernel; codec reports as "Fe-Pi Audio").
- SGTL5000 capture routing: `Capture Mux` = `LINE_IN`; line-out is the TX jack.
- Bench bringup + gain-staging origin: `docs/KNOWN_BUGS.md::BUG-IONOS-PI5-CHEAP-DAC`,
  memory `project_ionos_live_bringup_2026_06_15`.
