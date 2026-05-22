package com.example.blebeacon

import android.content.Context
import java.nio.ByteBuffer

object LighthouseContract {
    val PROXIMITY_UUID: ByteArray = byteArrayOf(
        0x13, 0x57, 0x9B.toByte(), 0xDF.toByte(),
        0x13, 0x57, 0x9B.toByte(), 0xDF.toByte(),
        0xCA.toByte(), 0xFE.toByte(), 0xBA.toByte(), 0xBE.toByte(),
        0x12, 0x34, 0x56, 0x78
    )

    const val TOKEN_WINDOW_SECONDS = 180L

    val TOKENS: List<ByteArray> = listOf(
        byteArrayOf(0x60, 0x8F.toByte(), 0x03, 0x4E, 0x77, 0x12, 0x99.toByte(), 0x29, 0x31, 0x42, 0xDE.toByte(), 0x3C, 0x10, 0x77, 0x6A, 0x00),
        byteArrayOf(0x15, 0x22, 0xAC.toByte(), 0x55, 0x96.toByte(), 0x03, 0x44, 0xE2.toByte(), 0x6A, 0x99.toByte(), 0x89.toByte(), 0xD2.toByte(), 0x58, 0x61, 0xB0.toByte(), 0x7F),
        byteArrayOf(0xE4.toByte(), 0x81.toByte(), 0x37, 0x6C, 0xAD.toByte(), 0x08, 0x18, 0x44, 0xA8.toByte(), 0x20, 0x5C, 0x3E, 0x01, 0xC1.toByte(), 0x0B, 0xF2.toByte()),
        byteArrayOf(0x40, 0x13, 0xB8.toByte(), 0x89.toByte(), 0xFF.toByte(), 0x1B, 0x75, 0x4F, 0xD0.toByte(), 0x65, 0xC3.toByte(), 0x8B.toByte(), 0x44, 0x23, 0x10, 0x55)
    )
}

object PayloadEncoder {
    fun encodeClaim(userId4: ByteArray, token16: ByteArray): ByteArray {
        require(userId4.size == 4) { "User_ID must be 4 bytes" }
        require(token16.size == 16) { "Token must be 16 bytes" }
        return ByteBuffer.allocate(20).put(userId4).put(token16).array()
    }

    fun parseUserIdHex(hex8: String): ByteArray? {
        val clean = hex8.trim().replace(" ", "")
        if (clean.length != 8 || !clean.matches(Regex("[0-9a-fA-F]+"))) return null
        return clean.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    }
}

class TokenSelector(private val consumed: (Int) -> Boolean) {
    fun currentWindowTokenIndex(epochSeconds: Long): Int {
        val size = LighthouseContract.TOKENS.size
        return ((epochSeconds / LighthouseContract.TOKEN_WINDOW_SECONDS) % size).toInt()
    }

    fun selectAdvertisableToken(epochSeconds: Long): Int? {
        val start = currentWindowTokenIndex(epochSeconds)
        for (i in LighthouseContract.TOKENS.indices) {
            val idx = (start + i) % LighthouseContract.TOKENS.size
            if (!consumed(idx)) return idx
        }
        return null
    }
}

enum class MirrorState {
    IDLE,
    PROXIMITY_DETECTED,
    CLAIM_ADVERTISING,
    UNLOCK_CONFIRMED,
    FAILED,
    TIMEOUT
}

sealed class MirrorEvent {
    object ProximitySeen : MirrorEvent()
    object StartAdvertising : MirrorEvent()
    object UnlockSuccess : MirrorEvent()
    object Fail : MirrorEvent()
    object Timeout : MirrorEvent()
    object Reset : MirrorEvent()
}

class MirrorStateMachine {
    var state: MirrorState = MirrorState.IDLE
        private set

    fun on(event: MirrorEvent): MirrorState {
        state = when (state) {
            MirrorState.IDLE -> when (event) {
                MirrorEvent.ProximitySeen -> MirrorState.PROXIMITY_DETECTED
                else -> MirrorState.IDLE
            }
            MirrorState.PROXIMITY_DETECTED -> when (event) {
                MirrorEvent.StartAdvertising -> MirrorState.CLAIM_ADVERTISING
                MirrorEvent.Fail -> MirrorState.FAILED
                MirrorEvent.Timeout -> MirrorState.TIMEOUT
                else -> state
            }
            MirrorState.CLAIM_ADVERTISING -> when (event) {
                MirrorEvent.UnlockSuccess -> MirrorState.UNLOCK_CONFIRMED
                MirrorEvent.Fail -> MirrorState.FAILED
                MirrorEvent.Timeout -> MirrorState.TIMEOUT
                else -> state
            }
            MirrorState.UNLOCK_CONFIRMED,
            MirrorState.FAILED,
            MirrorState.TIMEOUT -> when (event) {
                MirrorEvent.Reset -> MirrorState.IDLE
                else -> state
            }
        }
        return state
    }
}

class MirrorPrefs(context: Context) {
    private val prefs = context.getSharedPreferences("lighthouse_mirror", Context.MODE_PRIVATE)

    fun saveUserIdHex(hex: String) = prefs.edit().putString("user_id_hex", hex).apply()
    fun userIdHex(): String = prefs.getString("user_id_hex", "") ?: ""

    fun markConsumed(index: Int) {
        val current = consumedSet().toMutableSet()
        current += index
        prefs.edit().putString("consumed", current.sorted().joinToString(",")).apply()
    }

    fun isConsumed(index: Int): Boolean = consumedSet().contains(index)

    private fun consumedSet(): Set<Int> {
        val raw = prefs.getString("consumed", "") ?: ""
        if (raw.isBlank()) return emptySet()
        return raw.split(",").mapNotNull { it.toIntOrNull() }.toSet()
    }

    fun saveLastMeta(meta: String) = prefs.edit().putString("last_meta", meta).apply()
    fun lastMeta(): String = prefs.getString("last_meta", "") ?: ""
}
