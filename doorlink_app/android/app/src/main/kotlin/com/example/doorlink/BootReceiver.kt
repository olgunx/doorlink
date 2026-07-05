package com.example.doorlink

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat

class BootReceiver : BroadcastReceiver() {
    private val logTag = "DoorLinkBoot"

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) {
            Log.i(logTag, "Boot completed. Checking for stored credentials...")
            
            val prefs = context.getSharedPreferences("doorlink_credentials", Context.MODE_PRIVATE)
            val userId = prefs.getString("user_id", "") ?: ""
            val espPubKey = prefs.getString("esp_pub_key", "") ?: ""

            if (userId.isNotBlank() && espPubKey.isNotBlank()) {
                Log.i(logTag, "Credentials found. Starting BeaconService.")
                val serviceIntent = Intent(context, BeaconService::class.java).apply {
                    putExtra(BeaconService.EXTRA_USER_ID, userId)
                    putExtra(BeaconService.EXTRA_ESP_PUB_KEY, espPubKey)
                }
                
                try {
                    ContextCompat.startForegroundService(context, serviceIntent)
                } catch (e: Exception) {
                    Log.e(logTag, "Failed to start BeaconService on boot", e)
                }
            } else {
                Log.i(logTag, "No credentials found. Skipping service start.")
            }
        }
    }
}
