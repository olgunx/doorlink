package com.example.doorlink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.NotificationCompat
import android.bluetooth.le.AdvertisingSetCallback
import java.security.KeyFactory
import java.security.interfaces.ECPublicKey
import java.security.PublicKey
import java.security.spec.ECPoint
import java.security.spec.ECParameterSpec
import java.security.spec.ECPublicKeySpec
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

class BeaconService : Service() {
    private val logTag = "DoorLinkBeacon"
    companion object {
        const val EXTRA_USER_ID = "extra_user_id"
        const val ACTION_STOP = "com.example.blebeacon.action.STOP"
        const val ACTION_DEBUG_STATUS = "com.example.blebeacon.action.DEBUG_STATUS"
        const val EXTRA_DEBUG_MESSAGE = "extra_debug_message"

        private const val MANUFACTURER_ID_CHALLENGE = 0x0144
        private const val MANUFACTURER_ID_RESPONSE = 0x0143
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "doorlink_bg"
        private const val ADVERTISE_HOLD_MS = 2500L
        private const val RESPONSE_RETRY_MS = 3000L
        private const val STATIC_UUID_HEX = "9f82c41d3b7a4291a1e6b5293d0cfa82"
        private const val ESP_PUBLIC_KEY_HEX = "0446812cfc171d1e1f3cc2bcfe33d057b359a0e7d0192f6072b0ef7c4bf3e0fdac9663c30067311505a45df3133bf4a10f1ec4730763b921d2e0678092eedf28ff"
    }

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var advertiser: BluetoothLeAdvertiser
    private var scanner: BluetoothLeScanner? = null
    private var advertiseCallback: AdvertiseCallback? = null
    private var userIdBytes: ByteArray = ByteArray(0)
    private var lastChallengeHex: String = ""
    private var activeChallengeBytes: ByteArray = ByteArray(0)
    private var activeResponseBytes: ByteArray = ByteArray(0)
    private val staticServiceUuid: ParcelUuid by lazy {
        ParcelUuid(Utils.uuidFrom16Bytes(hexStringToByteArray(STATIC_UUID_HEX)))
    }
    private var serviceStarted = false

    private val responseRetryRunnable = object : Runnable {
        override fun run() {
            if (!serviceStarted) return
            if (activeChallengeBytes.isNotEmpty() && activeResponseBytes.isNotEmpty()) {
                if (advertiseCallback == null) {
                    advertiseResponse(activeChallengeBytes, activeResponseBytes, false)
                }
                handler.postDelayed(this, RESPONSE_RETRY_MS)
            }
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handleScanResult(result)
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            results.forEach { handleScanResult(it) }
        }

        override fun onScanFailed(errorCode: Int) {
            emitDebug("BLE scan failed: $errorCode")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(logTag, "service onStartCommand action=${intent?.action.orEmpty()} startId=$startId flags=$flags")
        if (intent?.action == ACTION_STOP) {
            emitDebug("Stop requested")
            stopEverything()
            stopSelf(startId)
            return START_NOT_STICKY
        }

        val userIdHex = intent?.getStringExtra(EXTRA_USER_ID)?.trim().orEmpty()
        if (userIdHex.isBlank()) {
            emitDebug("Missing credentials")
            stopSelf(startId)
            return START_NOT_STICKY
        }

        userIdBytes = hexStringToByteArray(userIdHex)
        if (userIdBytes.size != 4) {
            emitDebug("Invalid user id")
            stopSelf(startId)
            return START_NOT_STICKY
        }

        if (!serviceStarted) {
            startForeground(NOTIFICATION_ID, createNotification("DoorLink background active"))
            serviceStarted = true
            handler.removeCallbacks(responseRetryRunnable)
            handler.postDelayed(responseRetryRunnable, RESPONSE_RETRY_MS)
        }

        ensureBluetooth()
        startProximityScan()
        return START_STICKY
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(logTag, "service onCreate")
    }

    private fun ensureBluetooth() {
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter ?: throw IllegalStateException("Bluetooth adapter unavailable")
        advertiser = adapter.bluetoothLeAdvertiser ?: throw IllegalStateException("BLE advertiser unavailable")
        scanner = adapter.bluetoothLeScanner
    }

    private fun startProximityScan() {
        if (!hasScanPermissions()) {
            emitDebug("Missing BLE scan permissions")
            return
        }

        val filters = emptyList<ScanFilter>()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        scanner?.startScan(filters, settings, scanCallback)
        emitDebug("BLE background scan started")
        emitDebug("BLE scan requested with ${filters.size} filters, low-latency mode")
    }

    private fun handleScanResult(result: ScanResult) {
        val record = result.scanRecord ?: return
        val challengeBytes = record.getManufacturerSpecificData(MANUFACTURER_ID_CHALLENGE) ?: return
        if (challengeBytes.size < 8) return

        val challenge = challengeBytes.copyOfRange(0, 8)
        val challengeHex = bytesToHex(challenge)
        if (challengeHex == lastChallengeHex) return
        lastChallengeHex = challengeHex

        val response = computeResponse(userIdBytes, challenge) ?: run {
            emitDebug("Response computation failed")
            return
        }
        activeChallengeBytes = challenge
        activeResponseBytes = response
        advertiseResponse(challenge, response, true)
    }

    private fun advertiseResponse(challenge: ByteArray, response: ByteArray, emitLog: Boolean) {
        stopAdvertising()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            checkSelfPermission(android.Manifest.permission.BLUETOOTH_ADVERTISE) != PackageManager.PERMISSION_GRANTED
        ) {
            emitDebug("Missing BLUETOOTH_ADVERTISE permission")
            return
        }

        val payload = userIdBytes + challenge + response
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(false)
            .build()

        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(false)
            .setIncludeTxPowerLevel(false)
            .addManufacturerData(MANUFACTURER_ID_RESPONSE, payload)
            .build()

        val scanResponse = AdvertiseData.Builder()
            .addManufacturerData(MANUFACTURER_ID_RESPONSE, payload)
            .build()

        advertiseCallback = object : AdvertiseCallback() {
            override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {}

            override fun onStartFailure(errorCode: Int) {
                Log.e(logTag, "advertiseResponse start failure errorCode=$errorCode")
                emitDebug("Response advertising failed: $errorCode")
            }
        }
        advertiser.startAdvertising(settings, data, scanResponse, advertiseCallback)
        if (emitLog) {
            emitDebug("Response advertising started")
        }

        handler.postDelayed({ stopAdvertising() }, ADVERTISE_HOLD_MS)
    }

    private fun ensureKeyPair(): java.security.KeyPair? {
        return AppKeyMaterial.getOrCreate(this)
    }

    private fun computeResponse(userId: ByteArray, challenge: ByteArray): ByteArray? {
        return try {
            val keyPair = ensureKeyPair() ?: return null
            val espPublic = decodePublicKey(ESP_PUBLIC_KEY_HEX)
            val secret = deriveSharedSecret(keyPair.private, espPublic)
            if (secret.isEmpty()) return null
            val mac = Mac.getInstance("HmacSHA256")
            mac.init(SecretKeySpec(secret, "HmacSHA256"))
            mac.update(userId)
            mac.update(challenge)
            val response = mac.doFinal().copyOfRange(0, 15)
            response
        } catch (exception: Exception) {
            Log.e(logTag, "computeResponse failed", exception)
            null
        }
    }

    private fun decodePublicKey(hex: String): PublicKey {
        val bytes = hexStringToByteArray(hex)
        require(bytes.size == 65 && bytes[0] == 0x04.toByte()) {
            "Expected uncompressed P-256 public key (65 bytes, 0x04 prefix)"
        }

        val keyFactory = KeyFactory.getInstance("EC")
        val params = (AppKeyMaterial.getOrCreate(this).public as ECPublicKey).params
        return keyFactory.generatePublic(
            ECPublicKeySpec(
                ECPoint(
                    bytesToBigInt(bytes, 1, 32),
                    bytesToBigInt(bytes, 33, 32)
                ),
                params
            )
        )
    }

    private fun bytesToBigInt(bytes: ByteArray, offset: Int, length: Int): java.math.BigInteger {
        return java.math.BigInteger(1, bytes.copyOfRange(offset, offset + length))
    }

    private fun deriveSharedSecret(privateKey: java.security.PrivateKey, publicKey: PublicKey): ByteArray {
        val agreement = KeyAgreement.getInstance("ECDH")
        agreement.init(privateKey)
        agreement.doPhase(publicKey, true)
        return agreement.generateSecret()
    }

    private fun stopAdvertising() {
        if (advertiseCallback != null) {
            try {
                advertiser.stopAdvertising(advertiseCallback)
            } catch (_: Exception) {
            }
        }
        advertiseCallback = null
    }

    private fun stopEverything() {
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        stopAdvertising()
        handler.removeCallbacksAndMessages(null)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
        serviceStarted = false
        activeChallengeBytes = ByteArray(0)
        activeResponseBytes = ByteArray(0)
        lastChallengeHex = ""
    }

    override fun onDestroy() {
        Log.i(logTag, "service onDestroy")
        super.onDestroy()
    }

    private fun hasScanPermissions(): Boolean {
        val connectOk = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        val scanOk = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        val locationOk = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ||
            checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        return connectOk && scanOk && locationOk
    }

    private fun createNotification(text: String): Notification {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(CHANNEL_ID, "DoorLink background", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(channel)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("DoorLink")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
    }

    private fun emitDebug(message: String) {
        sendBroadcast(Intent(ACTION_DEBUG_STATUS).apply {
            setPackage(packageName)
            putExtra(EXTRA_DEBUG_MESSAGE, message)
        })
    }

    private fun hexStringToByteArray(s: String): ByteArray {
        val clean = s.trim().lowercase()
        if (clean.length % 2 != 0) return ByteArray(0)
        val data = ByteArray(clean.length / 2)
        for (i in clean.indices step 2) {
            val hi = Character.digit(clean[i], 16)
            val lo = Character.digit(clean[i + 1], 16)
            if (hi < 0 || lo < 0) return ByteArray(0)
            data[i / 2] = ((hi shl 4) + lo).toByte()
        }
        return data
    }

    private fun bytesToHex(bytes: ByteArray): String {
        return bytes.joinToString("") { "%02x".format(it) }
    }

    override fun onBind(intent: Intent?): IBinder? = null
}

object Utils {
    fun uuidFrom16Bytes(bytes: ByteArray): java.util.UUID {
        require(bytes.size == 16)
        var msb = 0L
        var lsb = 0L
        for (i in 0..7) msb = (msb shl 8) or (bytes[i].toLong() and 0xff)
        for (i in 8..15) lsb = (lsb shl 8) or (bytes[i].toLong() and 0xff)
        return java.util.UUID(msb, lsb)
    }
}
