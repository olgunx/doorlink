package com.example.doorlink

import java.security.KeyPair
import java.security.interfaces.ECPublicKey

object EcKeyEncoding {
    fun publicKeyHex(keyPair: KeyPair): String = publicKeyHex(keyPair.public as ECPublicKey)

    fun publicKeyHex(publicKey: ECPublicKey): String {
        val x = publicKey.w.affineX.toByteArray().let { normalizeTo32Bytes(it) }
        val y = publicKey.w.affineY.toByteArray().let { normalizeTo32Bytes(it) }
        val raw = ByteArray(65)
        raw[0] = 0x04
        System.arraycopy(x, 0, raw, 1, 32)
        System.arraycopy(y, 0, raw, 33, 32)
        return raw.joinToString("") { "%02x".format(it) }
    }

    private fun normalizeTo32Bytes(input: ByteArray): ByteArray {
        if (input.size == 32) return input
        val output = ByteArray(32)
        val start = if (input.isNotEmpty() && input[0] == 0.toByte() && input.size > 32) 1 else 0
        val copyLength = minOf(32, input.size - start)
        System.arraycopy(input, input.size - copyLength, output, 32 - copyLength, copyLength)
        return output
    }
}
