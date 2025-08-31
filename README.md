# TTGO T-Journal ESP32 Camera AP Stream

This project turns the **LilyGO TTGO T-Journal (ESP32 + OV2640 + OLED)** into a **standalone Wi-Fi camera**.  
It starts its own Wi-Fi hotspot (AP mode) and serves **live MJPEG video** directly to your phone or laptop via a browser — no apps required.

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
