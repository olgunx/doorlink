# DoorLink Mobile Companion App

DoorLink is a secure, offline-first smart access control application built with Flutter.

## 🚀 Running on PC & Simulation Modes

You can launch and test the application on PC using any of the following commands:

### 1. Chrome Web Browser (Web Simulation)
Runs the app instantly in Google Chrome:
```bash
flutter run -d chrome
```

### 2. Linux Desktop (Native PC App)
Compiles and launches as a native Linux desktop application:
```bash
flutter run -d linux
```

### 3. Android Emulator (Full Native & BLE Service Simulation)
Launch an Android virtual device and run the full native application:
```bash
# Launch Android emulator
flutter emulators --launch Medium_Phone_API_35

# Run application
flutter run
```

## 📱 Hardware / Physical Android Device

Connect an Android smartphone via USB with USB Debugging enabled, then execute:
```bash
flutter run
```

## 🛠️ Diagnostics & Code Quality

Run static analysis across Dart code:
```bash
flutter analyze
```
