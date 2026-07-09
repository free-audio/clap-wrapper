# AUv3 Implementation State

## Summary

The AUv3 wrapper is **release-ready** and is the most capable AU format in the project, surpassing AUv2 in bypass, timers, note expressions, parameter management, and GUI handling.

---

## Feature Comparison: AUv2 vs AUv3

| Feature | AUv2 | AUv3 | Notes |
|---------|------|------|-------|
| Audio Processing | Full | Full | |
| Multi-bus I/O | Full | Full | AUv3 has per-bus render deduplication |
| Parameters | Full | Full | AUv3 has hierarchical AUParameterGroup trees |
| Parameter Automation | Full | Full | |
| Parameter Rescan | Basic | Full | AUv3 handles ALL/INFO/VALUES flags |
| MIDI Input (MIDI 1.0) | Full | Full | |
| MIDI Input (MIDI 2.0) | N/A | Stubbed | Deferred |
| MIDI Output | Full | Full | |
| State Save/Restore | Full | Full | |
| Latency Reporting | Full | Full | |
| Tail Time Reporting | Full | Full | |
| GUI/Editor | Full | Full | AUv3 uses AUViewController |
| GUI Resize | Partial | Full | AUv3 uses KVO on preferredContentSize |
| Transport/Timing | Full | Full | |
| Bypass | Stubbed | Full | AUv3 wires shouldBypassEffect to CLAP_PARAM_IS_BYPASS |
| Factory Presets | Not implemented | Not implemented | Both rely on state save/restore |
| CLAP Timer Extension | Not implemented | Full | AUv3 fires from idle GCD timer |
| Note Expression (input) | Not implemented | Full | Poly AT, channel pressure, pitch bend |
| Note Expression (output) | Dropped | Full | Pressure and tuning back to MIDI |
| Active Note Tracking | Unused infrastructure | Active | Tracks by key+channel |
| Channel Capabilities | SupportedNumChannels | shouldChangeToFormat | Functionally equivalent |
| Build System | Full | Full | appex + standalone |
| Standalone Host | N/A (RtAudio) | Full | AVAudioEngine, tests full AUv3 round-trip |
| Parameter Ramp | N/A | Treated as instant | Minor |
| Context Menu | Stubbed | Stubbed | Parity |

---

## AUv3 Advantages Over AUv2

1. **Bypass** -- Full bidirectional bypass via `shouldBypassEffect` wired to `CLAP_PARAM_IS_BYPASS`. AUv2's bypass is stubbed.
2. **CLAP Timers** -- `register_timer()`/`unregister_timer()` with proper firing. AUv2 returns false.
3. **Note Expressions** -- Converts poly aftertouch, channel pressure, pitch bend to/from CLAP note expressions. AUv2 passes raw MIDI only.
4. **Parameter Hierarchy** -- Proper `AUParameterGroup` trees from CLAP module paths. AUv2 uses flat clumps.
5. **Parameter Rescan** -- Handles `CLAP_PARAM_RESCAN_ALL`, `INFO`, `VALUES` with KVO-based tree rebuilding.
6. **GUI Resize** -- Clean KVO on `preferredContentSize` vs AUv2's custom property hack.
7. **Multi-bus Processing** -- Per-bus render deduplication via sample-time tracking.
8. **Standalone Host** -- Tests the real AUv3 API round-trip, unlike AUv2 standalone which loads CLAP directly.

---

## Remaining Work (Non-blocking)

| Item | Priority | Notes |
|------|----------|-------|
| MIDI 2.0 / UMP | Nice to have | `AURenderEventMIDIEventList` not yet translated; would unlock all 7 note expression types |
| Parameter Ramp | Nice to have | `AURenderEventParameterRamp` treated as instant; could split into sub-block events |
| Factory Presets | Nice to have | Neither AU format exposes CLAP factory presets |
| iOS | Deferred | Code has `#if TARGET_OS_IPHONE` guards; needs UIKit adaptation |
| Context Menus | Deferred | Stubbed in both AUv2 and AUv3 |
