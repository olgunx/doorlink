package com.example.doorlink

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import android.util.Base64
import androidx.annotation.NonNull
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.spec.ECGenParameterSpec
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec

class MainActivity : FlutterActivity() {
    private val channelName = "com.example.blebeacon/lighthouse"
    private lateinit var methodChannel: MethodChannel

    private val debugReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val message = intent?.getStringExtra(BeaconService.EXTRA_DEBUG_MESSAGE).orEmpty()
            if (message.isNotBlank()) {
                methodChannel.invokeMethod("onNativeDebug", message)
            }
        }
    }

    override fun configureFlutterEngine(@NonNull flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        methodChannel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
        methodChannel.setMethodCallHandler { call, result ->
            when (call.method) {
                "getAppPublicKey" -> {
                    try {
                        result.success(EcKeyEncoding.publicKeyHex(AppKeyMaterial.getOrCreate(this)))
                    } catch (e: Exception) {
                        result.error("ERR_KEYPAIR", e.message, null)
                    }
                }
                "getStoredCredentials" -> {
                    val prefs = getSharedPreferences("doorlink_credentials", Context.MODE_PRIVATE)
                    val userId = prefs.getString("user_id", "") ?: ""
                    val espPubKey = prefs.getString("esp_pub_key", "") ?: ""
                    result.success(mapOf("userId" to userId, "espPubKey" to espPubKey))
                }
                "saveStoredCredentials" -> {
                    val userId = call.argument<String>("userId")?.trim().orEmpty()
                    val espPubKey = call.argument<String>("espPubKey")?.trim()
                    if (userId.isBlank()) {
                        result.error("ERR_BAD_ARGS", "Missing userId", null)
                        return@setMethodCallHandler
                    }
                    val edit = getSharedPreferences("doorlink_credentials", Context.MODE_PRIVATE).edit()
                    edit.putString("user_id", userId)
                    if (espPubKey != null) edit.putString("esp_pub_key", espPubKey)
                    edit.apply()
                    result.success("OK")
                }
                "startBackgroundService" -> {
                    val userId = call.argument<String>("userId")?.trim().orEmpty()
                    val espPubKey = call.argument<String>("espPubKey")?.trim().orEmpty()
                    if (userId.isBlank()) {
                        result.error("ERR_BAD_ARGS", "Missing userId", null)
                        return@setMethodCallHandler
                    }
                    if (espPubKey.isBlank()) {
                        result.error("ERR_BAD_ARGS", "Missing espPubKey", null)
                        return@setMethodCallHandler
                    }

                    val intent = Intent(this, BeaconService::class.java).apply {
                        putExtra(BeaconService.EXTRA_USER_ID, userId)
                        putExtra(BeaconService.EXTRA_ESP_PUB_KEY, espPubKey)
                    }
                    ContextCompat.startForegroundService(this, intent)
                    result.success("OK")
                }

                "stopBackgroundService" -> {
                    val intent = Intent(this, BeaconService::class.java).apply {
                        action = BeaconService.ACTION_STOP
                    }
                    startService(intent)
                    result.success("OK")
                }

                "getAndroidSdkInt" -> result.success(Build.VERSION.SDK_INT)
                "isIgnoringBatteryOptimizations" -> {
                    val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
                    result.success(powerManager.isIgnoringBatteryOptimizations(packageName))
                }
                "openBatteryOptimizationSettings" -> {
                    val intent = Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    startActivity(intent)
                    result.success("OK")
                }
                else -> result.notImplemented()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        val filter = IntentFilter(BeaconService.ACTION_DEBUG_STATUS)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(debugReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(debugReceiver, filter)
        }
    }

    override fun onPause() {
        try {
            unregisterReceiver(debugReceiver)
        } catch (_: Exception) {
        }
        super.onPause()
    }
}

object AppKeyMaterial {
    private const val PREFS_NAME = "doorlink_app_keys"
    private const val PUBLIC_KEY_B64 = "public_key_b64"
    private const val PRIVATE_KEY_B64 = "private_key_b64"

    fun getOrCreate(context: Context): KeyPair {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val publicB64 = prefs.getString(PUBLIC_KEY_B64, null)
        val privateB64 = prefs.getString(PRIVATE_KEY_B64, null)
        if (!publicB64.isNullOrBlank() && !privateB64.isNullOrBlank()) {
            try {
                val keyFactory = KeyFactory.getInstance("EC")
                val publicKey = keyFactory.generatePublic(X509EncodedKeySpec(Base64.decode(publicB64, Base64.DEFAULT)))
                val privateKey = keyFactory.generatePrivate(PKCS8EncodedKeySpec(Base64.decode(privateB64, Base64.DEFAULT)))
                return KeyPair(publicKey, privateKey)
            } catch (_: Exception) {
                prefs.edit().remove(PUBLIC_KEY_B64).remove(PRIVATE_KEY_B64).apply()
            }
        }

        val generator = KeyPairGenerator.getInstance("EC")
        generator.initialize(ECGenParameterSpec("secp256r1"))
        val keyPair = generator.generateKeyPair()
        prefs.edit()
            .putString(PUBLIC_KEY_B64, Base64.encodeToString(keyPair.public.encoded, Base64.NO_WRAP))
            .putString(PRIVATE_KEY_B64, Base64.encodeToString(keyPair.private.encoded, Base64.NO_WRAP))
            .apply()
        return keyPair
    }

}
