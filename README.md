# TTGO T-Journal ESP32 Camera AP Stream

This project turns the **LilyGO TTGO T-Journal (ESP32 + OV2640 + OLED)** into a **standalone Wi-Fi camera**.  
It starts its own Wi-Fi hotspot (AP mode) and serves **live MJPEG video** directly to your phone or laptop via a browser — no apps required.

![TTGO T-Journal OLED](doc/LILYGO%20TTGO%20T-Journal_normal_01-1500x1500w.jpg)

![TTGO T-Journal](doc/LILYGO%20TTGO%20T-Journal_normal_02-1500x1500.jpg)

---

## ✨ Features

- 📷 Live MJPEG video streaming over Wi-Fi (AP mode, no router needed)  
- 📶 Connect directly to ESP32 AP → `ESP32CAM-AP`  
- 🌐 Browser-based UI (http://192.168.4.1) with start/stop stream and settings  
- 📺 OLED display shows SSID and IP address  
- ⚡ Runs from USB power or single-cell Li-ion (e.g., 21700 with PCM)  
- 🔋 Runtime: ~14–18 h on a 4900 mAh Samsung 21700, >30 h with 2P pack  

---

## 🛠️ Hardware

- **LilyGO TTGO T-Journal** (ESP32, OV2640 camera, 0.91" OLED, SMA antenna)
- **Camera**: OV2640 (supplied with board)
- **Antenna**: 2.4 GHz SMA (supplied with board)
- **Power**:
  - Micro-USB (5 V from power bank or wall adapter), or
  - Single-cell Li-ion/Li-Po (e.g., Samsung 21700 4900 mAh with PCM protection) on JST-PH 2.0 connector
- (Optional) MicroSD card for snapshots / logging

---

## 📂 Project Structure

project/
├─ platformio.ini
├─ src/
│ └─ main.cpp
└─ README.md

- `platformio.ini`: PlatformIO configuration  
- `src/main.cpp`: Firmware code  
- `README.md`: This guide  

---

## ⚙️ Setup (PlatformIO + VSCode)

1. Install [Visual Studio Code](https://code.visualstudio.com)  
2. Install **PlatformIO IDE** extension  
3. Clone or copy this project folder  
4. Open the folder in VSCode (PlatformIO auto-detects `platformio.ini`)  
5. Connect TTGO T-Journal via micro-USB  
6. Build & Upload:
   - Click ✔️ (Build) then → (Upload) in the PlatformIO toolbar  
   - Or use shortcuts: `Ctrl+Alt+B` (build), `Ctrl+Alt+U` (upload)  

---

## 🔌 Powering

- **USB**: Connect to PC or power bank (5 V)  
- **Battery**: 3.7 V Li-ion on JST-PH (2-pin). Recommended:  
  - **Samsung 21700 (4900 mAh, 9.8 A)** with PCM  
  - Fuse (1.5–2 A) inline for safety  
- Board charges the battery via micro-USB (IP5306 PMU, ~1 A max)

---
# Tube Build — Option A (Single 21700, Inline)

This guide describes a compact, robust tube build for the **LilyGO TTGO T-Journal (ESP32 + OV2640 + OLED)** with a **single 21700 Li-ion cell** in-line behind the board. It’s designed for browser-based live video (AP mode) at **http://192.168.4.1** using the PlatformIO firmware in this repo.

---

## 1) Overview

- **Layout (front → back):** window/endcap → TTGO T-Journal (camera faces out) → wiring gap (PCM/BMS + fuse) → **1× 21700 cell** → rear endcap with **SMA bulkhead** (antenna) and **vent**.
- **Goal:** ~14–18 h runtime from a single 21700 (Samsung ~4900 mAh), robust range with SMA antenna, safe power with PCM + fuse.
- **Tube target size:** **Ø 30–32 mm ID**, **length 200–220 mm** (see dimensions below).

---

## 2) Bill of Materials (BOM)

**Core**
- 1× **LilyGO TTGO T-Journal** (ESP32, OV2640, 0.91" OLED, u.FL; SMA antenna usually included)
- 1× **Samsung 21700 Li-ion**, ~4900 mAh, **unprotected** (e.g., 50E/48G class)
- 1× **PCM/BMS (1S)**, 6–10 A rated (OVP/UVP/OCP/short protection)
- 1× **Inline fuse** (mini blade or polyfuse), **1.5–2 A** on battery +
- 1× **JST-PH 2.0 (2-pin) pigtail**, to match T-Journal battery header (check polarity!)
- 1× **u.FL → SMA female bulkhead** pigtail (if not included on board/kit)
- 1× **2.4 GHz SMA antenna** (stubby or whip)

**Enclosure**
- **Tube**: plastic or aluminum, **inner Ø 30–32 mm**, cut length **200–220 mm**
- **Front endcap** with **window**:
  - Option A: **round acrylic disk** Ø 25–28 mm, **2–3 mm** thick, bonded inside cap
  - Option B: 10–12 mm **aperture hole** (no window) if weatherproofing not required
- **Rear endcap**:
  - 1× **SMA bulkhead hole** ~**6.5 mm**
  - 1× **vent hole** **1–2 mm**, covered by **hydrophobic vent patch** (Gore-type) or mesh
- **O-rings / sealant**: appropriate for your tube + endcaps (e.g., nitrile O-rings, silicone gasket, or silicone sealant)
- **Mounting**: small **foam rails** or a **3D-printed sled** to center/secure board + cell

**Wiring & consumables**
- 22–24 AWG silicone wire (red/black)
- Heat-shrink tubing, Kapton tape
- Epoxy or RTV silicone (neutral cure) for window & strain relief
- Threadlocker (low strength) for SMA nut (optional)

**Tools**
- Drill bits: **6.5 mm** (SMA), **1–2 mm** (vent), **10–12 mm** (aperture if no window)
- Soldering iron, side cutters, deburring tool
- Calipers or ruler, marker for layout

---

## 3) Dimensions & Layout

**Known sizes**
- TTGO T-Journal PCB: approx **65 × 24 mm** (L × W)
- 21700 cell: **Ø 21 mm × 70 mm** (allow Ø 22 mm with wrap)

**Recommended tube**
- **Inner diameter (ID):** **30–32 mm**
  - Leaves space for 24 mm-wide board, 22 mm battery, foam, wiring
- **Usable internal length:** **≥ 180 mm**
  - 65 (board) + 70 (cell) + 20–30 (wiring/PCM/fuse) + ~15 (cap clearance)
- **Cut length:** **200–220 mm** (room for endcap intrusion and seals)

**Endcaps**
- **Front**: either a **clear window** bonded inside, or a **10–12 mm** aperture hole
- **Rear**:
  - **SMA bulkhead** center hole **~6.5 mm**
  - **Vent** hole **1–2 mm** (offset from center), covered with vent patch

---

## 4) Wiring (text schematic)

[21700 Cell] + ----> [PCM B+] (battery protection)
- ----> [PCM B-]

[PCM P+] --> [Fuse 1.5–2A] --> JST-PH + --> T-Journal BAT+
[PCM P-] ---------------------> JST-PH - --> T-Journal GND

Antenna: u.FL (board) --> u.FL-to-SMA pigtail --> SMA bulkhead (rear cap) --> SMA antenna


**Notes**
- Keep leads **short**. Add **heat-shrink** over joints.
- Verify **JST-PH polarity** before first power (Red=+, Black=– on board header).
- Bond/strain-relief the pigtail so the u.FL isn’t stressed.

---

## 5) Drilling Template (quick reference)

**Front cap (window)**
- Centered **round window**: Ø **25–28 mm** acrylic disk (2–3 mm thick) bonded from inside
  - Or: drilled aperture **Ø 10–12 mm** if no window is used

**Rear cap**
- **SMA bulkhead**: center drill **Ø 6.5 mm**
- **Vent**: offset drill **Ø 1–2 mm**, cover with vent patch/mesh

Deburr all holes. Test-fit SMA; ensure nut + washer seat cleanly.

---

## 6) Assembly Steps

1. **Prep the tube**
   - Cut to **200–220 mm**, deburr edges.
   - Dry-fit both endcaps.

2. **Front window (if used)**
   - Clean with IPA. Lightly scuff bonding surfaces.
   - Bond the **acrylic disk** inside the front cap using **RTV silicone (neutral cure)** or **thin epoxy**.  
   - Ensure the window is **flat** and **sealed** around edges. Let it cure fully.

3. **Rear cap pass-throughs**
   - Drill **6.5 mm** center hole for **SMA**; mount bulkhead (nut + washer).  
   - Drill **1–2 mm** **vent**; apply vent patch from inside (or mesh + breathable tape).

4. **Power harness**
   - Solder **PCM** to the 21700 cell (**B+ / B-**).  
   - From **PCM P+**, go to **fuse**, then to **JST-PH +**.  
   - **PCM P-** to **JST-PH –**.  
   - Heat-shrink and label polarity.
   - If shrinking the cell instead of a holder: add **insulating ring** on the + end; wrap neatly.

5. **Board & antenna**
   - Attach **u.FL → SMA** pigtail to the board.  
   - Route the pigtail to the rear cap (avoid sharp bends).  
   - Connect **JST-PH** to the T-Journal battery header (double-check polarity).  
   - Optionally display the **SSID/IP** on OLED when you first power up (bench test).

6. **Internal supports**
   - Use **foam rails** or a **3D-printed sled**:
     - Board channel width: **24 mm**
     - Battery channel diameter: **22 mm**
     - Provide **2–3 mm** foam on contact points for damping.
   - Leave **10–15 mm** **wiring slack** between board and battery (strain relief).

7. **Final fit**
   - Insert the board (camera forward), then battery, then the rear cap.  
   - Ensure the **camera lens** is centered behind the window/aperture.  
   - Check that antenna nut is snug (add a dab of threadlocker if desired).

8. **Sealing**
   - Use **O-rings** or a **silicone bead** in the cap grooves.  
   - Do **not** make the tube perfectly airtight — the **vent** is intentional.

---

## 7) Power, Charging & Safety

- **Protection**: always use a **PCM/BMS** with unprotected cells.
- **Fuse**: 1.5–2 A inline on **P+** after the PCM.
- **Charging**:
  - Easiest: via the **TTGO USB** (IP5306 on-board, ~1 A → long charge time for 4900 mAh).
  - Fastest/coolest: remove the cell and use a **dedicated 21700 Li-ion charger**.
- **Thermal**: do not run any onboard **flash LED** continuously inside a sealed space.
- **Vent**: keep the **1–2 mm vent** open and protected (hydrophobic patch).

---

## 8) RF & Performance Tips

- Place the **SMA antenna outside** the rear cap; avoid metal endcaps near the antenna.
- Keep **line-of-sight** when possible; avoid wrapping the antenna tightly along the tube.
- Start with **VGA** framesize; tune **SVGA/XGA** only if stream is stable.
- The **OLED** shows SSID/IP so you can connect quickly.

---

## 9) Bring-Up Checklist

- ✅ **Polarity** checked (JST-PH +/–)  
- ✅ **PCM** wired correctly: **B** to cell, **P** to load  
- ✅ **Fuse** installed on **P+**  
- ✅ **Antenna** connected (u.FL fully seated)  
- ✅ **Stream test on bench** (`http://192.168.4.1`) before sealing  
- ✅ **Vent** present and covered

---

## 10) Maintenance

- Periodically inspect **u.FL** connection and **SMA** nut.
- Check seals and the front **window** for fogging/cracks.
- Recharge before deep depletion; Li-ion longevity is best between **20–80%** SOC.
- If storing long term, leave the cell at **~50%** and power off.

---

## 📖 User Manual

1. **Power on the board**  
   - OLED shows boot info  
   - Starts Wi-Fi Access Point:  
     - **SSID**: `ESP32CAM-AP`  
     - **Password**: `camstream123`  
     - **IP**: `192.168.4.1`  

2. **Connect your phone/laptop to Wi-Fi**  
   - Open Wi-Fi settings → select `ESP32CAM-AP`  
   - Enter password  

3. **Open browser**  
   - Navigate to [http://192.168.4.1](http://192.168.4.1)  
   - You’ll see the control page  

4. **Control page functions**  
   - **Start Stream** → begins MJPEG stream  
   - **Stop Stream** → stops  
   - **Framesize** → QVGA, VGA, SVGA, XGA  
   - **Quality** → JPEG quality (lower = better image, bigger size)  

5. **OLED display**  
   - Shows SSID and IP for quick reference  
   - Updates on boot  

---

## 📡 Tips for Best Performance

- Use the **supplied SMA antenna** for reliable 50+ m range (line of sight).  
- For outdoor housing (tube design):
  - Place antenna at the **plastic endcap** for RF transparency  
  - Provide **vent/relief** for Li-ion safety (don’t seal airtight)  
- Lower framesize (QVGA/VGA) → smoother stream, lower bandwidth  
- Higher framesize (XGA) → sharper, but may drop frames  

---

## 🛑 Safety Notes

- Always use **protected Li-ion cells** or packs with a **PCM/BMS**.  
- Add a fuse inline with the positive lead (1.5–2 A).  
- Never charge a Li-ion cell in a completely sealed enclosure.  
- Observe polarity on JST-PH connector (Red = +, Black = –).  

---

## 📷 Demo

1. Power on → OLED shows `ESP32CAM-AP / 192.168.4.1`  
2. Connect phone Wi-Fi to `ESP32CAM-AP`  
3. Open browser: [http://192.168.4.1](http://192.168.4.1)  
4. Click **Start Stream** → see live video  

---

## 📌 License

Open-source demo project.  
Use and adapt freely for personal and educational projects.  
No warranty for safety, fitness, or reliability.

---
