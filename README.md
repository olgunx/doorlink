# DoorLink: Secure Zero-Interaction Smart Lock

DoorLink is a highly secure, proximity-based smart lock system built with an ESP32 and a Flutter/Android companion app. It is designed to allow zero-interaction "hands-free" unlocking using background Bluetooth Low Energy (BLE) while maintaining strict cryptographic security to prevent spoofing and replay attacks. 

To ensure the door only opens when you actually want to enter, the system requires both cryptographic authentication and physical intent (waving a hand over a laser sensor), with a robust manual-override fallback.

---

## 🏗️ System Architecture

The system consists of two primary components:
1. **The Mobile App (Digital Key):** A Flutter app with a native Android Kotlin background service that listens for BLE challenges and computes cryptographic responses seamlessly in the background, even when the phone is locked.
2. **The ESP32 Device (Lock Controller):** An ESP32 that broadcasts rotating cryptographic challenges, scans for authenticated responses, validates physical proximity, and triggers the door relay.

---

## 🔐 How It Works: High-Level Security

Standard smart locks often rely on slow Bluetooth connections or send static passwords over the air, which hackers can easily intercept and reuse to open your door later (known as a "Replay Attack"). 

DoorLink completely avoids standard Bluetooth pairings. Instead, it uses a lightning-fast **Challenge-Response** mechanism. Think of it as a secret handshake where the required password changes every few seconds.

Here is what happens behind the scenes:

1. **The Shared Secret (Key Exchange):** During initial setup, your phone and the lock securely agree on a master "Shared Secret" using military-grade cryptography (NIST P-256 ECDH). This master secret is safely stored on both devices and is **never** transmitted over the air.
2. **The Challenge (The Lock asks a question):** The lock constantly broadcasts a random mathematical puzzle (the "Challenge") to the surrounding area. It changes this puzzle every 15 seconds.
3. **The Response (The Phone answers):** The app running in the background of your pocketed phone hears the puzzle. It uses the master Shared Secret to solve the puzzle (using an HMAC-SHA256 hash) and silently broadcasts the correct answer back.
4. **The Verification (The Lock checks the work):** The lock calculates the answer itself using the master Shared Secret. When it receives your phone's broadcast, it verifies a perfect match. It also measures the Bluetooth signal strength (RSSI) to ensure you are standing directly in front of the door, not just parked in the driveway.
5. **Physical Intent (The Wave):** Just because you are authorized and nearby doesn't mean you want to go inside! The lock arms itself and waits for you to wave your hand over its laser distance sensor, proving your intent to enter.
6. **Anti-Replay Protection (One-Time Use):** Once the door opens, that specific puzzle is instantly "burned". Even if a hacker was hiding in the bushes and recorded your phone's answer, playing it back to the door a minute later won't work because the lock has already moved on to a new puzzle.

*(Note for developers: To bypass aggressive OS-level BLE duplicate filtering that prevents locked phones from seeing the same beacon twice, the ESP32 dynamically randomizes its own BLE MAC address every time the challenge rotates!)*

### 🧑‍💻 Code Deep-Dive: Keys in Action

If you look at the source code, here is exactly how the public and private keys are used to solve the "puzzle":

**1. The Mobile App (`BeaconService.kt`)**
When the Android app scans the BLE challenge, it runs the `computeResponse()` function:
* It grabs the **App's Private Key** (stored securely on the phone).
* It grabs the **ESP32's Public Key** (hardcoded for now as `ESP_PUBLIC_KEY_HEX`).
* It uses Elliptic-Curve Diffie-Hellman (`KeyAgreement.getInstance("ECDH")`) to combine them into a `Shared Secret`. 
* It then uses `HmacSHA256` to hash the `Shared Secret`, the `User ID`, and the `Challenge`, broadcasting the first 15 bytes back to the door.

**2. The ESP32 Lock (`app_main.c`)**
When the lock receives the broadcast, it runs the `compute_expected_response()` function:
* It grabs the **ESP32's Private Key** (from internal `device_key.c`).
* It looks up the **App's Public Key** tied to the User ID (from `enrollment_mgr.c`).
* It uses mbedTLS (`mbedtls_ecdh_compute_shared()`) to combine them. Thanks to the math of ECDH, combining the *Lock's Private Key* and the *App's Public Key* results in the **exact same `Shared Secret`**.
* It runs the same `HmacSHA256` hash on the Challenge. If its computed hash matches the one broadcasted by the phone, the door unlocks!

---

## 🛠️ Setting Up the Build Environment From Scratch

Follow these instructions to set up a complete development environment from a fresh, empty Ubuntu installation (Ubuntu 22.04 LTS or 24.04 LTS recommended) for both the ESP32 firmware and the mobile/desktop app.

### 0. System Prerequisites & Serial Permissions (Fresh Ubuntu Setup)

Before installing toolchains, configure user permissions for USB serial hardware (ESP32) and Android emulator virtualization:

```bash
# Add current user to dialout (for UART/serial access) and kvm (for Android emulator)
sudo usermod -a -G dialout,kvm $USER

# Remove brltty daemon if installed (prevents USB-Serial port conflicts on Ubuntu)
sudo apt remove -y brltty

# Re-login or reboot for group permissions to take effect:
# (Or run 'newgrp dialout' in your active terminal)
```

---

### 1. ESP32 Firmware Environment Setup (`doorlink_device`)

The firmware relies on **ESP-IDF (Espressif IoT Development Framework)** v5.x with CMake and GCC/Clang toolchains for RISC-V (`esp32c3`).

#### Step 1: Install System Dependencies (Ubuntu / Debian)
```bash
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0-dev
```

#### Step 2: Install ESP-IDF Toolchain
```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.1.2 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32c3
```

#### Step 3: Export ESP-IDF Environment Variables
Add ESP-IDF to your shell configuration (`~/.bashrc`) so `idf.py` is always available in new terminal sessions:
```bash
echo '. $HOME/esp/esp-idf/export.sh' >> ~/.bashrc
source ~/.bashrc
```

#### Step 4: CLI Build & Flash (Terminal Workflow)
```bash
# Navigate to root or doorlink_device folder
cd doorlink_device

# Set target microcontroller to ESP32-C3
idf.py set-target esp32c3

# Compile the firmware
idf.py build

# Flash to connected ESP32-C3 board and launch serial console monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

#### Step 5: Visual Studio Code Setup (IDE Workflow)

For developing, building, flashing, and monitoring UART logs directly inside VS Code:

1. **Install Recommended VS Code Extensions**:
   - **ESP-IDF Extension** (`espressif.esp-idf-extension`): Building, flashing, partition management, and serial output monitoring.
   - **C/C++ Tools** (`ms-vscode.cpptools`): IntelliSense, code navigation, and syntax highlighting.
   - **CMake Tools** (`ms-vscode.cmake-tools`): CMake project support.
   *(Opening `doorlink_device` in VS Code will prompt you to install these via [.vscode/extensions.json](file:///home/olgun/CODE/doorlink/doorlink_device/.vscode/extensions.json)).*

2. **Link Existing ESP-IDF Setup**:
   - Open Command Palette (`Ctrl+Shift+P` / `Cmd+Shift+P`).
   - Search for **ESP-IDF: Configure ESP-IDF Extension**.
   - Select **Use Existing Setup** and point it to your ESP-IDF directory (`~/esp/esp-idf`) and Python virtual environment (`~/.espressif/python_env/...`).

3. **Building, Flashing & Monitoring via VS Code Status Bar**:
   - **Set Target Device**: Click target icon in status bar (or run `ESP-IDF: Set Espressif Device Target`) and select `esp32c3`.
   - **Select Serial Port**: Click port icon in status bar (or run `ESP-IDF: Select Port to Use`) and select your serial port (`/dev/ttyUSB0` or `/dev/ttyACM0`).
   - **Build**: Click the Cylinder icon ⚙️ (or press `Ctrl+E B` / `ESP-IDF: Build Project`).
   - **Flash**: Click the Lightning icon ⚡ (or press `Ctrl+E F` / `ESP-IDF: Flash Device`).
   - **Monitor UART Logs**: Click the Monitor icon 🖥️ (or press `Ctrl+E M` / `ESP-IDF: Monitor Device`).
   - **All-in-One Shortcut**: Click **Build, Flash and Start Monitor** on the bottom status bar for 1-click execution.

---

### 2. Mobile & Desktop App Environment Setup (`doorlink_app`)

The companion app is built using **Flutter** (Dart SDK `^3.0.0`) with native **Android (Kotlin, Java 17)** support for the background BLE service, as well as **Linux Desktop** and **Web (Chrome)** target environments.

#### Step 1: Install Java Development Kit (JDK 17)
```bash
sudo apt install -y openjdk-17-jdk
echo 'export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64' >> ~/.bashrc
source ~/.bashrc
```

#### Step 2: Install Flutter SDK
```bash
# Clone Flutter SDK (stable channel)
cd ~
git clone https://github.com/flutter/flutter.git -b stable

# Add Flutter binary to PATH persistently
echo 'export PATH="$PATH:$HOME/flutter/bin"' >> ~/.bashrc
source ~/.bashrc

# Pre-download development tools & binaries
flutter doctor
```

#### Step 3: Install Android Studio & SDK Components
1. Download and install [Android Studio](https://developer.android.com/studio).
2. Open Android Studio -> **SDK Manager** -> **SDK Tools**, and verify installation of:
   - **Android SDK Build-Tools**
   - **Android SDK Command-line Tools**
   - **Android SDK Platform-Tools**
   - **NDK (Side by side)**
3. Accept Android licenses:
   ```bash
   flutter doctor --android-licenses
   ```

#### Step 4: Install Linux Desktop Build Dependencies (Optional for Desktop Debugging)
```bash
sudo apt install -y clang cmake ninja-build pkg-config libgtk-3-dev liblzma-dev
```

#### Step 5: Fetch Dependencies & Verify Setup
```bash
cd doorlink_app

# Fetch Dart/Flutter package dependencies
flutter pub get

# Run Flutter doctor to verify full toolchain setup
flutter doctor
```

#### Step 6: Build & Launch
```bash
# Run on connected Android smartphone or running emulator:
flutter run

# Run on Linux Desktop:
flutter run -d linux

# Run on Chrome Browser (Web Simulation):
flutter run -d chrome

# Build production Android release APK:
flutter build apk --release
```

---

## 📱 Mobile App (Flutter / Android)

### 💻 Running on PC & Simulation Modes

DoorLink can be executed directly on a PC for testing, UI evaluation, and development using three simulation environments:

1. **Chrome Web Browser (Web Simulation)**:
   ```bash
   cd doorlink_app
   flutter run -d chrome
   ```

2. **Linux Desktop (Native PC Build)**:
   ```bash
   cd doorlink_app
   flutter run -d linux
   ```

3. **Android Emulator (Full Native & BLE Service Simulation)**:
   ```bash
   # Launch available Android emulator
   flutter emulators --launch Medium_Phone_API_35

   # Run the app
   cd doorlink_app
   flutter run
   ```

### Background Beacon Service (`BeaconService.kt`)
The heart of the mobile app is a native foreground service that runs continuously.
* **Low-Latency Scanning:** It constantly scans for the ESP32's specific Manufacturer Data (`0x0144`) containing the rotating challenge.
* **Stateless Broadcasting:** Upon calculating the HMAC response, it acts as a BLE Advertiser itself, broadcasting the response payload (`0x0143`) for a brief window (2.5 seconds).
* **Split Payload Design:** To bypass the strict 31-byte BLE packet limit, the service places the manufacturer payload in the primary `AdvertiseData` packet, and the Service UUID in the `ScanResponse` packet.

### Manual Unlock Fallback
If the user wants to unlock the door without triggering the physical laser sensor, they can tap "Manual Unlock" in the app. This utilizes a highly reliable **Dual-Path Trigger**:
1. **Wi-Fi Fast Path:** The app attempts an HTTP POST request to the ESP32's local Access Point (`http://192.168.4.1/api/unlock`).
2. **BLE Fallback Shift:** If Wi-Fi fails, the app briefly shifts its background BLE Service UUID from `0xFCD2` to `0xFCD3` for 5 seconds. The ESP32 detects this UUID shift and flags the manual override.

---

## ⚙️ ESP32 Firmware

### 2. Main Logic & Intent Verification (`app_main.c`)
Opening the door requires three conditions to be met simultaneously:
1. **Authorized (`s_authorized`):** The cryptographic HMAC response must match the expected value computed by the ESP32.
2. **Arrived (`s_arrived`):** The RSSI (signal strength) of the phone's BLE response must be stronger than a defined threshold (e.g., `-127` dBm, configurable), ensuring the user is physically near the door.
3. **Intent / Laser Detected (`s_laser_detected`):** A VL53L0X Time-of-Flight laser sensor continuously polls distance. The user must wave their hand in front of the sensor (distance > 0 && distance <= threshold). This ensures the door doesn't pop open just because someone walked past the inside of the door with their phone in their pocket.
   * *Override:* If the `Manual Unlock` button was pressed via the app, this laser requirement is bypassed for a 5-second window.

### 3. Web Administration Console (`web_console.c`)
The ESP32 broadcasts a hidden Wi-Fi network (`DL_DOOR`). By connecting to this network and navigating to `http://192.168.4.1`, administrators can access a secure portal.
* **Login:** Protected by an admin password verified against NVS storage.
* **Device Provisioning:** Admins can view the ESP32's internal Public Key, and register new users by pasting the *Device Fingerprint (User ID)* and *Phone Public Key* generated by the Flutter app.
* **Revocation:** Allows admins to instantly delete and revoke access for specific users.

---

## 🔄 Step-by-Step Workflows

### 1. Provisioning a New Phone
1. Open the DoorLink Flutter app. A unique P-256 Keypair and 4-byte User ID (Fingerprint) are generated automatically.
2. The user taps **"Share Fingerprint with Admin"**.
3. The Admin connects to the `DL_DOOR` Wi-Fi AP, opens `192.168.4.1`, logs in, and registers the User ID and Public Key.
4. The Admin provides the ESP32's Public Key back to the user to enter into their app. The Shared Secret is now established.

### 2. Auto-Unlock (Zero Interaction)
1. **Approach:** You walk up to the door with your phone locked in your pocket or bag.
2. **Listen & Solve:** The phone silently hears the lock's current "puzzle" (challenge), solves it using its hidden key, and shouts the correct answer.
3. **Verify:** The lock hears the correct answer and measures the signal strength to ensure you are standing right in front of the door (not 30 feet away).
4. **Intent:** The lock arms itself. You wave your hand over the laser sensor to prove you actually want to open the door.
5. **Unlock:** The door unlocks, and the lock immediately changes the puzzle so the same answer can never be used again.

---

## 📝 TODO: Commercial & Architectural Roadmap (Türkiye Market Focus)

To transition DoorLink from a functional prototype into a mass-market, multi-tenant B2B/B2C product for Turkish apartments (*apartman*) and residential facilities, the following tasks must be completed:

### 🛠️ Hardware & Industrial Design Changes
- [ ] **Split-Module Enclosure Design:**
  - **Main Controller Box:** House the ESP32, relay circuitry, and power terminal inside a secure, indoor-rated box installed safely inside the building lobby. It will only require a standard 12V DC adapter power entry.
  - **External Sensor Block:** Create a tiny, ruggedized, weatherproof (IP65+) outdoor pod containing only the VL53L0X Time-of-Flight sensor.
  - **Inter-Module Wiring:** Connect the main box and the outdoor sensor block via a robust, vandal-resistant 4-pin cable.
- [ ] **Intercom (Diyafon) Parallel Integration:** Design the dry-contact relay output terminal to wire directly in parallel with existing building intercom systems (Audio, Netelsan, Mas, etc.) to trigger the 12V door strike (*kapı otomatiği*) without interfering with indoor flat-to-door buzzer pulses.

### 📱 Usability & Admin Onboarding Overhaul (Zero-Friction UX)
- [ ] **Remove Web Console Dependency:** Phase out the local Wi-Fi Access Point (`web_console.c`) approach for user provisioning to fit non-technical building managers (*apartman yöneticisi*).
- [ ] **Native OS Share Sheet Integration (Flutter):** Implement OS-level sharing receivers (`receive_sharing_intent` or `share_handler`) in the mobile app.
  - **Image Share Target:** Allow the admin to open a user-submitted QR code screenshot directly from WhatsApp, click "Share", select DoorLink, and have the app automatically parse the user ID and public key.
  - **Text Share Target:** Allow the admin to share encrypted invitation text/deep-links directly from WhatsApp into the app.
- [ ] **Background Passive Sync:** Program the admin's app to silently queue newly approved residents. When the manager passes by the main building door, when the app connects to the ESP32 over a secure administrator BLE GATT characteristic and push the new credentials to `enrollment_mgr.c` within seconds.

### 🔐 Cryptographic Offline Licensing & Time-Verification Architecture
Because the device lacks internet access, a Real-Time Clock ($RTC$) chip, or a battery backup, implement a bulletproof asymmetrically signed license enforcement protocol:
- [ ] **Hardware-Locked Activation Tokens:**
  - **Unique Device Identification:** On initial setup, the Flutter app reads the ESP32's immutable 6-byte factory MAC address / Chip ID (e.g., `DL-A87F-99C2`).
  - **License Generation (Vendor-Side):** The management board pays the annual fee and provides their Chip ID. The vendor uses an offline **Licensing Private Key** (Ed25519 or ECDSA) to sign a payload payload containing:
    $$\text{Payload} = [\text{Chip ID}] + [\text{License Issue Date}] + [\text{License Expiry Date (Unix Timestamp)}]$$
  - This outputs an alpha-numeric activation string (e.g., `X7R9-A2B1-K9LM-3P4Q`).
- [ ] **On-Chip Decryption & Validation (ESP32-Side):**
  - Embed the vendor’s public **Licensing Verification Key** inside the ESP32 firmware.
  - When the admin pastes the token into the app, it is piped via BLE directly to the ESP32.
  - The ESP32 decrypts the token, mathematically validates the vendor signature, and confirms the payload's `Chip ID` perfectly matches its own physical chip register. If copied to another building's lock, it triggers a critical hardware mismatch and rejects the operation.
- [ ] **Crowdsourced Monotonically Increasing Time Engine (Anti-Tampering):**
  - **Time Tracking without RTC:** Store a `Last_Known_Timestamp` variable in the ESP32 Non-Volatile Storage ($NVS$).
  - **Passive Clock Injection:** Every single time an authorized resident or admin triggers the door lock via BLE, their mobile app securely transmits the phone's current Unix epoch timestamp as an auxiliary encrypted parameter.
  - **Strict Monotonic Rule:** The ESP32 compares the incoming smartphone timestamp against the stored `Last_Known_Timestamp`. If the smartphone time is greater, the ESP32 advances its clock forward. If a malicious user sets their personal smartphone clock backward to trick the lock into an unexpired license window, the ESP32 detects that time is flowing backward and drops the update.
  - **Expiration Check:** When the monotonically advanced `Last_Known_Timestamp` exceeds the `License Expiry Date` decrypted from the activation token, the ESP32 safely toggles into a locked administrative state until a new valid token is fed.
- [ ] **Firmware Hardening:** Enable ESP32-C3 hardware-level **Secure Boot** to prevent side-channel JTAG flash modifications of the licensing conditions, and **Flash Encryption** to protect the local keys and user storage tables in raw memory.

### 📊 Market & Distribution Channel Alignment
- [ ] **B2B Electrician Sales Channel Strategy:** Price the system to target local neighborhood electricians and building security installers. Ensure the installation requires no software configuration for the installer (Pure plug-and-play: 12V adapter in + 2 wires to the door lock strike button).
- [ ] **Per-Household Cost Competitiveness:** Price the hardware unit between **3,500 TL – 5,500 TL**, positioning it as a highly economic alternative to cheap, cloneable RFID fobs (*göstergeç*). For a standard 20-unit apartment block, this drops the per-flat investment below the 300 TL cost of a single physical fob, while providing infinite, cost-free digital key issuance.
- [ ] **Recurring Revenue Architecture:** Handle renewals strictly at the building-management level via the custom offline license activation framework, shielding individual tenants from individual microtransactions.
