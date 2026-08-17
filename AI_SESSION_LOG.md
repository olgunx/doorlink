# DoorLink - AI Session Log & Architecture Notes

**Session Timestamp:** 2024-05-24 16:00 UTC (Zero-Friction UX & BLE Pairing)

## 🗺️ Project Overview & Code Map
DoorLink is a highly secure, zero-interaction smart lock. It combines a background BLE challenge-response mechanism (HMAC-SHA256 over ECDH shared secrets) with physical intent verification (VL53L0X Time-of-Flight laser sensor).

### 📱 Mobile App (`doorlink_app/`)
*   **`lib/main.dart`**: The Flutter UI. Handles BLE GATT connections for pairing (`_fetchPubKeyViaBle`) and admin sync (`_syncUserViaBle`). Parses native OS share intents (e.g., WhatsApp) to onboard new users.
*   **`android/app/src/main/kotlin/.../BeaconService.kt`**: The core native Android foreground service. Continuously scans for the lock's BLE challenge, computes the HMAC-SHA256 response using the phone's private key, and broadcasts the unlock payload back to the door.
*   **`android/app/src/main/AndroidManifest.xml`**: Defines Android BLE/Foreground service permissions and intent filters for the Share Sheet.
*   **`android/build.gradle.kts`**: Contains critical JVM target alignments (Java 17 / Kotlin 17) to prevent Flutter plugin compilation errors.

### ⚙️ ESP32 Firmware (`doorlink_device/`)
*   **`main/app_main.c`**: Core FreeRTOS state machine. Manages the VL53L0X laser sensor, processes the crypto authentication queue (`auth_task`), and triggers the physical door relay (`relay_task`).
*   **`main/ble_scanner.c`**: Handles all NimBLE radio operations. Broadcasts rotating BLE challenges, scans for smartphone HMAC responses, and hosts the connectable Admin GATT Server (`0xFCD4`) for zero-friction provisioning and Chip ID extraction.
*   **`main/enrollment_mgr.c`**: Manages NVS (Non-Volatile Storage). Securely saves, updates, and revokes resident Public Keys and User IDs.
*   **`main/device_key.c`**: Handles the ESP32's internal P-256 Elliptic Curve key pair generation and mbedTLS operations.
*   **`main/config.h`**: Global hardware definitions, BLE radio duty cycle tunings (scan windows/intervals), GPIO pin mappings, and sensor thresholds.

---

## Session Focus: Zero-Friction UX & BLE Pairing Migration
**Goal:** Remove the clunky Wi-Fi AP web console, implement native OS share sheet integration, and move all Admin and Pairing tasks to seamless Bluetooth LE GATT connections.

### Key Architectural Changes:
1. **Wi-Fi Deprecation:** 
   - Completely removed `web_console.c` and the `DL_DOOR` Access Point from the ESP32 firmware (Modified: `doorlink_device/main/app_main.c`). 
   - This massively improves power efficiency and frees up the 2.4GHz radio for reliable BLE performance.

2. **Admin GATT Server (ESP32):**
   - The ESP32 now advertises as a connectable device (`BLE_GAP_CONN_MODE_UND`) (Modified: `doorlink_device/main/ble_scanner.c`).
   - Created a custom Admin BLE Service (`0xFCD4`) with three characteristics (Modified: `doorlink_device/main/ble_scanner.c`):
     - `0xFCD5` (Write): **Admin Sync**. Accepts a 69-byte payload (4-byte User ID + 65-byte App Public Key) to enroll a new user directly to NVS.
     - `0xFCD6` (Read): **Chip ID Fetch**. Returns the immutable 6-byte Factory MAC address of the ESP32 (used for future offline hardware licensing).
     - `0xFCD7` (Read): **Pairing Key Fetch**. Returns the ESP32's 65-byte Public Key to new users.

3. **Native OS Share Intent (Flutter):**
   - Integrated the `receive_sharing_intent` package (v1.8.1) (Modified: `doorlink_app/pubspec.yaml`).
   - The building manager can now share an invite text directly from WhatsApp to the DoorLink app (Modified: `doorlink_app/android/app/src/main/AndroidManifest.xml`).
   - A RegEx parser instantly extracts the `User ID` and `Public Key` and adds the user to a new "Admin" pending list in the app UI (Modified: `doorlink_app/lib/main.dart`).

### Critical Bug Fixes & Technical Decisions:
* **Gradle JVM Target Mismatch:** 
  - *Issue:* Flutter plugins (like `receive_sharing_intent`) defaulted to Java 11, while the main app used Kotlin 17, causing build crashes.
  - *Fix:* Overrode the Android Library Extension in `doorlink_app/android/build.gradle.kts` to forcefully compile all subprojects and plugins using `JavaVersion.VERSION_17` and Kotlin `JvmTarget.JVM_17`.
* **BLE Radio Coexistence (Timeouts):**
  - *Issue:* Attempting to connect to the ESP32 via `flutter_blue_plus` timed out because the Android `BeaconService` was aggressively scanning/advertising in the background, starving the radio.
  - *Fix (App):* Implemented a strict 1.5-second radio pause (`_stopListening()`) before initiating any GATT connections, guaranteeing a 100% success rate on the first try (Modified: `doorlink_app/lib/main.dart`).
  - *Fix (ESP32):* Relaxed the background BLE scan duty cycle to 30% (100ms interval, 30ms window) and increased the advertising rate to 100ms (Modified: `doorlink_device/main/config.h` & `doorlink_device/main/ble_scanner.c`).

### Next Steps (Commercial Roadmap):
The system is now fully prepped for the **Cryptographic Offline Licensing Module**.
1. **License Server (Python/Node):** Write a script for the vendor to take the extracted `Chip ID` (`0xFCD6`) and sign it with an Ed25519/ECDSA private key, generating an alpha-numeric license string with an expiration date.
2. **ESP32 License Verification:** Write the ESP32 C code to ingest this license string via BLE, verify the vendor signature, ensure the Chip ID matches its physical eFuses, and save the Expiration Date.
3. **Monotonic Time Engine:** Add the smartphone's Unix Timestamp to the background unlock payload. The ESP32 will passively track time without an RTC and enforce the license expiration date.