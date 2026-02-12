# Configuration System

**Purpose:** Document current configuration objects and where each one is applied.

---

## Configuration Layers

| Layer | Scope | Persistence | Source |
|-------|-------|-------------|--------|
| `AppSettings` | Operator/UI preferences | INI file | `src/gui/widgets/settings.hpp/cpp` |
| `ModemConfig` | DSP/waveform parameters | Runtime object | `include/ultra/types.hpp` |
| Negotiated link mode | Waveform/mod/rate per connection | Runtime protocol state | `src/protocol/*` + `src/gui/app.cpp` |

---

## AppSettings (Persisted User Settings)

File path:
- Linux/macOS: `~/.config/ultra/settings.ini`
- Windows: `%APPDATA%\ProjectUltra\settings.ini`

Main fields:
- Station: `callsign`, `grid_square`, `name`
- Audio: `input_device`, `output_device`, `tx_delay_ms`, `tx_tail_ms`, `tx_drive`
- Filter: `filter_enabled`, `filter_center`, `filter_bandwidth`, `filter_taps`
- File transfer: `receive_directory` (empty => platform Downloads path)
- Expert overrides: `forced_waveform`, `forced_modulation`, `forced_code_rate` (`0xFF` = AUTO)

`AppSettings::save()` and `AppSettings::load()` handle INI serialization directly.

---

## ModemConfig (DSP Parameters)

Defined in `include/ultra/types.hpp`.

Important defaults in the `ModemConfig` struct:
- `sample_rate = 48000`
- `center_freq = 1500`
- `fft_size = 1024`
- `num_carriers = 59`
- `cp_mode = MEDIUM`
- `symbol_guard = 0`
- `modulation = QPSK`
- `code_rate = R1_2`
- `use_pilots = false` (legacy flag)
- `pilot_spacing = 2` (legacy spacing, overridden in connected OFDM)

Helper methods include:
- `getCyclicPrefix()`
- `getSymbolDuration()`
- `getSymbolRate()`
- `getDataCarriers()`
- `getTheoreticalThroughput(...)`

---

## Presets (`namespace ultra::presets`)

Current preset helpers in `include/ultra/types.hpp`:
- `conservative()`
- `balanced()`
- `turbo()`
- `high_throughput()`
- `high_speed()`

Operational default used by `ModemEngine` constructor:
- `config_ = presets::balanced()`
- balanced currently sets `DQPSK + R1/2` on 1024 FFT / 59 carriers profile.

---

## Runtime Effective Defaults

There are two important levels:

1. Type defaults (`ModemConfig` struct literal defaults).
2. Runtime defaults after component setup (`ModemEngine` / `App` constructors).

In practice, app startup uses:
- `App` loads `AppSettings`
- `ModemEngine` initializes from `presets::balanced()`
- `ProtocolEngine` receives expert overrides from settings (`AUTO` unless forced)
- Filter and output gain are applied from settings

---

## Negotiation And Mode Application

Mode/rate application path:

```text
Protocol negotiates waveform + modulation + code rate
  -> App callback (setDataMode / setModeNegotiated)
  -> ModemEngine updates encoder/decoder config
```

Current OFDM pilot behavior in connected mode:
- `ModemEngine::setDataMode()` and `setConnected(true)` force `use_pilots = true`.
- Pilot spacing is rate-dependent:
  - `R3/4` -> spacing `15` (lighter pilots)
  - other active OFDM rates (`R2/3`, `R1/2`, `R1/4`) -> spacing `10`

This behavior intentionally matches waveform encoder/decoder layout and should be treated as source of truth over older docs.

---

## Expert Overrides

Expert settings are applied to protocol negotiation:
- `setPreferredMode(...)`
- `setForcedModulation(...)`
- `setForcedCodeRate(...)`

Semantics:
- `0xFF` means AUTO (normal adaptive behavior)
- Any explicit value forces that choice at negotiation time

These overrides are persisted in INI and restored on startup.

---

## Enumerations (Reference)

Primary enums are defined in:
- `include/ultra/types.hpp` (`Modulation`, `CodeRate`, others)
- `src/protocol/frame_v2.hpp` (`WaveformMode`)

Notable currently-supported values include:
- Modulation: `DBPSK`, `BPSK`, `DQPSK`, `QPSK`, `D8PSK`, `QAM8`, `QAM16`, `QAM32`, `QAM64`, `QAM256`, `AUTO`
- Code rate: `R1_4`, `R1_3`, `R1_2`, `R2_3`, `R3_4`, `R5_6`, `R7_8`, `AUTO`

---

## Settings Flow Summary

```text
Load settings.ini
  -> apply callsign + expert options to ProtocolEngine
  -> apply filter/output gain to Modem/Audio
  -> run

User changes settings UI
  -> SettingsWindow callback
  -> apply immediately (protocol/audio/modem)
  -> settings_.save()
```

---

## Adding New Persistent Setting

1. Add field to `AppSettings` in `src/gui/widgets/settings.hpp`.
2. Add INI load/save handling in `src/gui/widgets/settings.cpp`.
3. Add UI control in `SettingsWindow::render...`.
4. Add callback wiring in `src/gui/app.cpp` if runtime reconfiguration is needed.
