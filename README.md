# PulseView2

**Reviving the pulse in PulseView.**

**PulseView2** is an actively maintained fork of the open-source [sigrok](https://sigrok.org/) **PulseView** — the Qt-based logic analyzer, oscilloscope, and mixed-signal oscilloscope (MSO) graphical user interface.

Upstream PulseView has accumulated years of valuable community bugfixes, hardware improvements, and UI enhancements sitting unmerged across pull requests and independent forks. **PulseView2** exists to bring those improvements together into a cohesive, stable, modern build with automated releases — putting the *pulse* back into PulseView.

---

## Key Enhancements Over Upstream PulseView

### Device & Hardware Integration
- **Dynamic sample rate & sample count updates:** Automatically refreshes available sample rates and sample counts when digital/analog channels are toggled (critical for analyzers whose maximum rate depends on active channel count, such as Sipeed SLogic and Hantek devices).
- **Expanded device configuration bindings:** Added support for writable `SR_CONF_NUM_LOGIC_CHANNELS` and `SR_CONF_TEST_MODE` device options, plus live trace updates when device configuration changes.
- **Clean serial device lifecycle:** Properly closes hardware devices when a session is destroyed (preventing leaked/locked serial ports) and preserves already-open devices across driver rescans.
- **SLogic channel palette:** 8-color-per-group digital channel color palette for clearer grouping on SLogic analyzers.

### Waveform View, Cursors & UI Polish
- **Faster & cleaner cursor snapping:** Improved cursor snapping performance on large captures, with nearest-sample snapping (matching DSView behavior) when dragging cursors between edges.
- **Ruler context-menu fixes:** Fixed timestamp calculation for *Create Marker* and *Set Zero Position* from the ruler popup menu (previously read an invalidated mouse-down position).
- **Trigger-as-zero-time fix:** Fixed zero-offset calculation and reset behavior when captures stop with *Trigger is zero time* enabled.
- **Analog signal min/max fix:** Properly initializes and resets analog min/max display values on capture clear, eliminating garbage/NaN SI prefixes when autoranging is disabled.
- **Standardized platform shortcuts:** Migrated core menu shortcuts (`Open`, `Save`, `Close`, `Quit`) to Qt `QKeySequence::StandardKey` constants for native macOS (`Cmd`) and cross-platform consistency.
- **Toolbar segment-mode fix:** Fixed the *Display a single segment* toolbar action signal connection.
- **Desktop file integration:** Updated `org.sigrok.PulseView.desktop` (`Exec=pulseview %F`) so `.sr` sigrok session files open directly in PulseView from file managers.

### Stability, Memory Safety & Sessions
- **Clean session setup save/restore:** Clears stale keys when overwriting `.pvs` setup files and tears down existing secondary views, protocol decoders, and math signals before restoring — preventing duplicate signals on reload.
- **Protocol decoder shutdown safety:** Fixed a use-after-free race condition during decoder teardown (joining decoder threads before destroying the `srd` session), resolved a paused-restart deadlock, and zero-initialized decoder segment progress counters.
- **Segment allocation fault safety:** Hardened `Segment` chunk allocation and compaction with `std::unique_ptr` ownership semantics to prevent double-free / use-after-free on `std::bad_alloc` (verified with a 31-case fault-injection test suite).
- **Export thread error reporting:** Catches export worker thread creation failures in `StoreSession` and surfaces a proper UI error dialog instead of crashing.

### Packaging & Platform Support
- **Linux ARM64 & Debian packaging:** Native ARM64 build support and `.deb` packaging workflows.
- **Windows release automation:** Dedicated Windows `x86_64` CI build and GitHub Releases pipeline.

---

## Building from Source

PulseView2 builds with CMake, Qt 5 (or Qt 6), Boost, `glibmm`, `libsigrokcxx`, and `libsigrokdecode` (targeting `libsigrok` / `libsigrokdecode` `master`):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

---

## Upstream Project & Credits

PulseView2 is built directly on the work of the [sigrok](https://sigrok.org/wiki/PulseView) project and its contributors, incorporating fixes and enhancements from community contributors and forks (including Sipeed, Terstegge, PentHertz, yuzhe-TheFool, and upstream `sigrokproject/pulseview` pull requests).

- **Upstream Website:** https://sigrok.org/wiki/PulseView
- **Upstream Repository:** https://github.com/sigrokproject/pulseview
- **Upstream Mailing List:** https://lists.sourceforge.net/lists/listinfo/sigrok-devel
- **IRC:** `#sigrok` on Libera.Chat

---

## License & Third-Party Resources

PulseView2 is licensed under the **GNU General Public License (GPL), version 3 or later** (GPLv3+). While some individual source files are licensed under GPLv2+, GPLv3+, or MIT, the application as a whole is licensed under GPLv3+. Please see individual source files for the full list of copyright holders.

### Bundled Third-Party Resources
- **Tango Icon Library** (`icons/application-exit.png`, `document-new.png`, `document-open.png`, `document-save-as.png`, `edit-paste.svg`, `help-browser.png`, `media-playback-pause.png`, `media-playback-start.png`, `preferences-system.png`, `search.svg`, `window-new.png`, `zoom-fit-best.png`, `zoom-in.png`, `zoom-out.png`) — Public Domain
- **Information icon** (`icons/information.svg`, Bobarino) — GFDL 1.2+ / CC-BY-SA 3.0
- **Add Math Channel icon** (`icons/add-math-channel.svg`, Inductiveload) — Public Domain
- **QDarkStyleSheet** (Colin Duquesnoy) — CC-BY 4.0
- **DarkStyle** (Juergen Skrotzky) — MIT License
- **QHexView** (Victor Anjin) — MIT License
- **ExprTk** (Arash Partow) — MIT License
