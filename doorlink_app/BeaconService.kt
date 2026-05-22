package com.example.blebeacon

import android.Manifest
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
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.NotificationCompat

class BeaconService : Service() {
    private val logTag = "BeaconService"

    companion object {
        const val EXTRA_USER_ID = "extra_user_id"
        const val ACTION_STOP = "com.example.blebeacon.action.STOP"
        const val ACTION_MARK_SUCCESS = "com.example.blebeacon.action.MARK_SUCCESS"
        const val ACTION_DEBUG_STATUS = "com.example.blebeacon.action.DEBUG_STATUS"
        const val EXTRA_DEBUG_DIRECT_ADVERTISE = "extra_debug_direct_advertise"
        const val EXTRA_DEBUG_MESSAGE = "extra_debug_message"

        private const val MANUFACTURER_ID = 0x0143
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "ble"
        private const val RETRY_BACKOFF_MS = 3_000L
        private const val TOKEN_CYCLE_MS = 1000L
    }

    private lateinit var advertiser: BluetoothLeAdvertiser
    private var scanner: BluetoothLeScanner? = null
    private var advertiseCallback: AdvertiseCallback? = null
    private val handler = Handler(Looper.getMainLooper())
    private val fsm = MirrorStateMachine()
    private lateinit var prefs: MirrorPrefs

    private var retryCount = 0
    private var activeTokenIndex: Int? = null
    private var tokenCycleIndex = 0

    private val tokenCycleRunnable = object : Runnable {
        override fun run() {
            if (fsm.state != MirrorState.CLAIM_ADVERTISING) return
            startAdvertisingForToken(tokenCycleIndex)
            tokenCycleIndex = (tokenCycleIndex + 1) % LighthouseContract.TOKENS.size
            handler.postDelayed(this, TOKEN_CYCLE_MS)
        }
    }

    private val timeoutRunnable = Runnable {
        fsm.on(MirrorEvent.Timeout)
        emitDebug("State=${fsm.state} session timeout")
        prefs.saveLastMeta("result=timeout token=${activeTokenIndex ?: -1}")
        stopAdvertising()
        handler.removeCallbacks(tokenCycleRunnable)
        scheduleReset()
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            if (result == null) return
            if (fsm.state == MirrorState.IDLE) {
                fsm.on(MirrorEvent.ProximitySeen)
                emitDebug("State=${fsm.state} lock proximity detected")
                startClaimAdvertisingFlow()
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        prefs = MirrorPrefs(this)

        if (intent?.action == ACTION_STOP) {
            emitDebug("Stop requested")
            cleanupAndStop(startId)
            return START_NOT_STICKY
        }

        if (intent?.action == ACTION_MARK_SUCCESS) {
            handleSuccess()
            return START_NOT_STICKY
        }

        val userIdHex = intent?.getStringExtra(EXTRA_USER_ID)?.trim().orEmpty()
        if (userIdHex.isBlank()) {
            emitDebug("Missing user id")
            cleanupAndStop(startId)
            return START_NOT_STICKY
        }
        prefs.saveUserIdHex(userIdHex)

        val manager = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter
        advertiser = adapter?.bluetoothLeAdvertiser ?: run {
            emitDebug("BLE advertiser unavailable")
            cleanupAndStop(startId)
            return START_NOT_STICKY
        }
        scanner = adapter.bluetoothLeScanner

        val debugDirectAdvertise = intent?.getBooleanExtra(EXTRA_DEBUG_DIRECT_ADVERTISE, false) == true
        if (debugDirectAdvertise) {
            startForeground(NOTIFICATION_ID, createNotification("Debug direct advertise mode"))
            emitDebug("Debug direct-advertise enabled; skipping proximity scan")
            fsm.on(MirrorEvent.ProximitySeen)
            startClaimAdvertisingFlow()
        } else {
            startForeground(NOTIFICATION_ID, createNotification("Waiting for lock proximity"))
            startProximityScan()
        }
        return START_STICKY
    }

    private fun startProximityScan() {
        if (!hasScanPermissions()) {
            emitDebug("Missing BLE scan permissions")
            return
        }

        val uuid = ParcelUuid(Utils.uuidFrom16Bytes(LighthouseContract.PROXIMITY_UUID))
        val filters = listOf(ScanFilter.Builder().setServiceUuid(uuid).build())
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner?.startScan(filters, settings, scanCallback)
        emitDebug("State=${fsm.state} scanning for static UUID")
    }

    private fun startClaimAdvertisingFlow() {
        val userIdBytes = PayloadEncoder.parseUserIdHex(prefs.userIdHex())
        if (userIdBytes == null) {
            fsm.on(MirrorEvent.Fail)
            emitDebug("State=${fsm.state} invalid user id format (need 8 hex chars)")
            return
        }

        val selector = TokenSelector { idx -> prefs.isConsumed(idx) }
        val firstIdx = selector.selectAdvertisableToken(System.currentTimeMillis() / 1000L)
        if (firstIdx == null) {
            fsm.on(MirrorEvent.Fail)
            emitDebug("State=${fsm.state} no available token (all consumed)")
            return
        }

        fsm.on(MirrorEvent.StartAdvertising)
        if (fsm.state != MirrorState.CLAIM_ADVERTISING) {
            emitDebug("State=${fsm.state} cannot start advertising; expected CLAIM_ADVERTISING")
            return
        }
        tokenCycleIndex = firstIdx
        emitDebug("State=${fsm.state} advertising token cycle started at index=$firstIdx")
        updateNotification("Always-on claim advertising")
        handler.removeCallbacks(tokenCycleRunnable)
        tokenCycleRunnable.run()
    }

    private fun startAdvertisingForToken(idx: Int) {
        val userIdBytes = PayloadEncoder.parseUserIdHex(prefs.userIdHex()) ?: return
        activeTokenIndex = idx
        val payload = PayloadEncoder.encodeClaim(userIdBytes, LighthouseContract.TOKENS[idx])
        emitDebug("State=${fsm.state} tokenIndex=$idx advertising=on")
        startAdvertising(payload)
    }

    private fun startAdvertising(payload: ByteArray) {
        stopAdvertising()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE) != PackageManager.PERMISSION_GRANTED
        ) {
            emitDebug("Missing BLUETOOTH_ADVERTISE permission")
            return
        }

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(false)
            .build()

        val data = AdvertiseData.Builder()
            .addManufacturerData(MANUFACTURER_ID, payload)
            .build()

        advertiseCallback = object : AdvertiseCallback() {
            override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
                emitDebug(
                    "BLE advertise started: mode=${settingsInEffect?.mode} " +
                        "tx=${settingsInEffect?.txPowerLevel} connectable=${settingsInEffect?.isConnectable}"
                )
            }

            override fun onStartFailure(errorCode: Int) {
                val reason = when (errorCode) {
                    ADVERTISE_FAILED_ALREADY_STARTED -> "ALREADY_STARTED"
                    ADVERTISE_FAILED_DATA_TOO_LARGE -> "DATA_TOO_LARGE"
                    ADVERTISE_FAILED_FEATURE_UNSUPPORTED -> "FEATURE_UNSUPPORTED"
                    ADVERTISE_FAILED_INTERNAL_ERROR -> "INTERNAL_ERROR"
                    ADVERTISE_FAILED_TOO_MANY_ADVERTISERS -> "TOO_MANY_ADVERTISERS"
                    else -> "UNKNOWN"
                }
                emitDebug("BLE advertise failed: code=$errorCode reason=$reason")
            }
        }
        advertiser.startAdvertising(settings, data, advertiseCallback)
        emitDebug("BLE advertise request submitted: mfgId=0x${MANUFACTURER_ID.toString(16)} payloadLen=${payload.size}")
    }

    private fun handleSuccess() {
        val idx = activeTokenIndex
        if (idx == null) {
            emitDebug("Success ignored; no active token")
            return
        }
        prefs.markConsumed(idx)
        fsm.on(MirrorEvent.UnlockSuccess)
        emitDebug("State=${fsm.state} tokenIndex=$idx consumed")
        prefs.saveLastMeta("result=success token=$idx")
        stopAdvertising()
        handler.removeCallbacks(tokenCycleRunnable)
        handler.removeCallbacks(timeoutRunnable)
        scheduleReset()
    }

    private fun scheduleReset() {
        retryCount += 1
        val delay = (retryCount * RETRY_BACKOFF_MS).coerceAtMost(15_000L)
        emitDebug("Retry backoff ${delay}ms")
        handler.postDelayed({
            fsm.on(MirrorEvent.Reset)
            activeTokenIndex = null
            emitDebug("State=${fsm.state} advertising=off")
            updateNotification("Waiting for lock proximity")
        }, delay)
    }

    private fun stopAdvertising() {
        if (::advertiser.isInitialized && advertiseCallback != null) {
            advertiser.stopAdvertising(advertiseCallback)
        }
        advertiseCallback = null
    }

    private fun cleanupAndStop(startId: Int) {
        handler.removeCallbacksAndMessages(null)
        stopAdvertising()
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        stopSelf(startId)
    }

    override fun onDestroy() {
        cleanupAndStop(0)
        super.onDestroy()
    }

    private fun hasScanPermissions(): Boolean {
        val connectOk = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        val scanOk = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        return connectOk && scanOk
    }

    private fun createNotification(text: String): Notification {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(CHANNEL_ID, "BLE Beacon", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(channel)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Lighthouse Mirror")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .build()
    }

    private fun updateNotification(message: String) {
        val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIFICATION_ID, createNotification(message))
    }

    private fun emitDebug(message: String) {
        Log.i(logTag, message)
        sendBroadcast(Intent(ACTION_DEBUG_STATUS).apply {
            setPackage(packageName)
            putExtra(EXTRA_DEBUG_MESSAGE, message)
        })
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
