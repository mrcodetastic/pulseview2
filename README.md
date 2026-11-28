# PulseView2

**Bringing the pulse back to PulseView.**

**PulseView2** is an actively maintained fork of the open-source [sigrok](https://sigrok.org/) **PulseView** — the Qt 6-based logic analyzer, oscilloscope, and mixed-signal oscilloscope (MSO) graphical user interface.

Upstream PulseView accumulated years of community bugfixes, hardware improvements, and usability enhancements that were never merged, while modern toolchains and operating systems moved on. **PulseView2** consolidates those fixes and adds major new capabilities — a modern `.pv2` workspace format, live hover frequency/pulse measurements, a full-capture overview minimap, searchable protocol decoder tables synced with the waveform, safe multi-session hardware sharing, and automated cross-platform Qt 6 releases.

---

## Highlights & Enhancements Over Upstream PulseView

### Interactive Analysis & Navigation
- **Live Hover Measurements (No Cursors Required):** Hover over any digital pulse to immediately see **frequency**, **period (`T`)**, **pulse width (`High`/`Low`)**, and **duty cycle (%)** in a floating HUD with pulse/period highlighting — no manual A/B cursor placement needed.
- **Full-Capture Overview Minimap:** A 100px+ overview strip docked at the bottom of the Trace View renders the entire capture across all enabled logic channels plus trigger markers. Click anywhere to jump, drag the highlighted window to pan, or drag its edges / scroll to zoom.
- **Searchable Protocol Decoder Table + Waveform Sync:** Open **View → Protocol Decoder Table** (`Ctrl+Shift+D`) to filter decoded frames across all columns in real time; single-clicking any row immediately focuses and zooms the main waveform view on that packet.
- **Compress Idle Intervals:** Collapse long dead/idle stretches between bursts of signal activity so bursts can be inspected side-by-side.
- **Faster & Smarter Cursor Snapping:** Improved cursor snapping on large captures with nearest-sample snapping between edges, fixed ruler context-menu timestamps, and *Trigger is zero time* alignment.

### Modernized UI, Tabs & Workflow
- **Clearer Menu Bar & File Organization:** Full desktop menu bar (`File`, `Capture`, `View`, `Tools`, `Help`), live status bar, and compact toolbars without redundant clutter.
- **Session Tab Badges & Custom Renaming:** Each session tab shows a live color-coded status dot (capturing, awaiting trigger, ready, shared-device busy, loaded file, or disconnected) and device tooltip. Double-click or right-click any tab to rename it — and at least one capture session tab always remains open.
- **Pop-Out Views & Independent Channel Sizing:** Detach any view into a native OS window, and adjust channel row heights independently (`Ctrl++`, `Ctrl+-`, `Ctrl+0`) from horizontal time-axis zoom (`+` / `-`).
- **Classic Channel Palette:** Preserves the familiar resistor-code digital channel colors with automatic high-contrast label text.

### New `.pv2` Workspace Format (Replacing `.pvs`)
- **Self-Contained `.pv2` Archives (`Ctrl+Shift+S` / `Ctrl+Shift+O`):** A ZIP-backed format containing a structured `pulseview.json` manifest (channel names, colors, order, decoders, math channels, triggers, views, zoom/scroll, cursors, flags, custom tab names, device identity, sample rate, sample limits, and continuous mode) plus **optional embedded capture data (`.sr`)** (checked by default).
- **Full Workspace or Single-Session Profiles:** Save all open tabs via **File → Save Workspace...** or export a single tab as a reusable preset/profile via **File → Save Current Workspace Session...**. **Open Workspace...** auto-detects whether the `.pv2` is a full workspace or a single session.
- **Clearer Capture Data Menu:** Replaced ambiguous `Save` / `Save As...` / `.pvs` menus with **Open Capture Data...** (`Ctrl+O`), **Save Capture Data...** (`Ctrl+S`), and **Save Selected Capture Data...** (`Ctrl+R`).

### Hardware, Multi-Session & Device Improvements
- **Safe Multi-Session Device Sharing:** Multiple session tabs can reference the same physical hardware device without stealing datafeeds or corrupting capture state. While one session captures, peer sessions sharing that hardware clearly show **Busy**; sessions on *different* devices capture independently. New sessions automatically inherit the prior session's connected hardware device.
- **Beginner-Friendly Driver List & Recent Devices:** The **Connect to Device** dialog pins `fx2lafw` at the top as **`Generic 24MHz 8CH Logic Analyzer / FX2 (fx2lafw)`**, sorts all other drivers A–Z by display name, and remembers **Recent Devices** for one-click **Reconnect**.
- **Preserve Capture on USB Reconnect:** Reconnecting a dropped analyzer rebinds matching channels in place so existing waveforms, decoders, and view state are preserved.
- **Dynamic Sample Rate & Count Updates:** Automatically refreshes supported sample rates and sample counts as channels are toggled, and supports writable `SR_CONF_NUM_LOGIC_CHANNELS` and `SR_CONF_TEST_MODE` configuration keys.

### Protocol Decoder Performance & Stability
- **High-Density Decoding:** Binary-search annotation lookups and pixel-bucketed level-of-detail (LOD) rendering keep the UI responsive even with hundreds of thousands of decoded annotations.
- **Hardened Lifecycle & Memory Safety:** Fixed protocol decoder shutdown races, paused-restart deadlocks, `std::thread` / memory allocation fault handling, and export error reporting.

---

## Cross-Platform Qt 6 Releases

Automated GitHub Releases bundle **PulseView2** with the full sigrok stack (`libsigrok`, `libsigrokdecode`, protocol decoders, and `fx2lafw` firmware):
- **Windows (`x86_64`):** Installer (`.exe`, registered in Windows *Add or remove programs*) and portable `.zip`
- **macOS (`arm64` / Apple Silicon):** Self-contained `.dmg` app bundle
- **Linux (Ubuntu 26.04 LTS `amd64`):** `.deb` package with `sigrok-cli` and udev rules

![Screenshot from Windows 11](pulseview_CCJKq72LBa.png)

---

## Building from Source

PulseView2 requires **C++17**, **CMake (>= 3.16)**, **Ninja**, **Qt 6 (>= 6.2)**, **Boost**, **`glibmm-2.4`**, **`libzip`**, **`libsigrokcxx`**, and **`libsigrokdecode`**:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

---

## Upstream Project & Credits

PulseView2 builds directly on the work of the [sigrok](https://sigrok.org/wiki/PulseView) project and incorporates fixes and enhancements from community contributors and forks (including Sipeed, Terstegge, PentHertz, yuzhe-TheFool, and upstream `sigrokproject/pulseview` pull requests).

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
- **QHexView** (Victor Anjin) — MIT License
- **ExprTk** (Arash Partow) — MIT License
