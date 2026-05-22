package com.example.doorlink

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
import androidx.annotation.NonNull
import androidx.core.app.ActivityCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.example.blebeacon/lighthouse"
    private var advertiser = BluetoothAdapter.getDefaultAdapter()?.bluetoothLeAdvertiser

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            if (call.method == "startMirroring") {
                val userIdHex = call.argument<String>("userId") ?: ""
                val status = runLighthouseMirror(userIdHex)
                result.success(status)
            } else {
                result.notImplemented()
            }
        }
    }

    private fun runLighthouseMirror(userIdHex: String): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return "ERR_OS_TOO_OLD"

        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            return "ERR_NO_LOCATION_PERM"
        }

        // 1. Scan Wi-Fi for Stealth Token
        val scanResults = wifiManager.scanResults
        if (scanResults.isEmpty()) return "ERR_WIFI_CACHE_EMPTY"

        var sawTarget = false
        var targetDebug = "IEs:"
        var token: ByteArray? = null

        for (scanResult in scanResults) {
            val isTargetAp = scanResult.SSID == "DL_DOOR"
            if (isTargetAp) sawTarget = true

            val ies = scanResult.informationElements ?: continue
            for (ie in ies) {
                if (isTargetAp) {
                    targetDebug += "${ie.id}"
                    if (ie.id == 221) {
                        val buf = ie.bytes.asReadOnlyBuffer()
                        val b = ByteArray(buf.remaining())
                        buf.get(b)
                        if (b.size >= 3) targetDebug += "[${String.format("%02X%02X%02X", b[0].toInt() and 0xFF, b[1].toInt() and 0xFF, b[2].toInt() and 0xFF)}-s${b.size}]"
                    }
                    targetDebug += ","
                }

                if (ie.id == 221) {
                    val buffer = ie.bytes.asReadOnlyBuffer()
                    val bytes = ByteArray(buffer.remaining())
                    buffer.get(bytes)
                    // Expecting: Apple OUI (00 17 F2) + OUI Type (42) + Token (6 bytes)
                    if (bytes.size >= 10 && bytes[0] == 0x00.toByte() && bytes[1] == 0x17.toByte() && bytes[2] == 0xF2.toByte() && bytes[3] == 0x42.toByte()) {
                        token = bytes.copyOfRange(4, 10)
                        break
                    }
                }
            }
            if (token != null) break
        }

        if (token == null) {
            return if (sawTarget) targetDebug else "ERR_AP_NOT_IN_CACHE"
        }

        // 2. Broadcast BLE Mirrored Claim
        val userIdBytes = hexStringToByteArray(userIdHex)
        if (userIdBytes.size != 4) return "ERR_INVALID_USER_ID"

        val padding = ByteArray(10) { 0x00 }
        val manufacturerData = userIdBytes + token + padding // User_ID (4) + Token (6) + Padding (10) = 20 bytes

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH) // Crucial for ESP32 -73dBm threshold
            .setConnectable(false)
            .build()

        val data = AdvertiseData.Builder()
            .addManufacturerData(0x0143, manufacturerData)
            .build()

        advertiser?.startAdvertising(settings, data, object : AdvertiseCallback() {})
        return "SUCCESS"
    }

    private fun hexStringToByteArray(s: String): ByteArray {
        val data = ByteArray(s.length / 2)
        for (i in s.indices step 2) {
            data[i / 2] = ((Character.digit(s[i], 16) shl 4) + Character.digit(s[i + 1], 16)).toByte()
        }
        return data
    }
}
