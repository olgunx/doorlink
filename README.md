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
4. **Physical Intent (The Wave):** Just because your phone is near the door doesn't mean you want to go outside! The lock verifies your phone's answer and measures the signal strength. It then waits for you to wave your hand over the laser sensor, proving your intent to open the door.
5. **Anti-Replay Protection (One-Time Use):** Once the door opens, that specific puzzle is instantly "burned". Even if a hacker was hiding in the bushes and recorded your phone's answer, playing it back to the door a minute later won't work because the lock has already moved on to a new puzzle.

*(Note for developers: To bypass aggressive OS-level BLE duplicate filtering that prevents locked phones from seeing the same beacon twice, the ESP32 dynamically randomizes its own BLE MAC address every time the challenge rotates!)*

---

## 📱 Mobile App (Flutter / Android)

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
3. **Intent / Laser Detected (`s_laser_detected`):** A VL6180X Time-of-Flight laser sensor continuously polls distance. The user must wave their hand in front of the sensor (distance > 0 && distance <= threshold). This ensures the door doesn't pop open just because someone walked past the inside of the door with their phone in their pocket.
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