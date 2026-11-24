package com.mw2.recomp

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * The root folder of an Xbox 360 disc image (XDVDFS), as tools/xdvdfs.py
 * and the runtime's installer (launcher/disc.cpp) read it: enough to find
 * a file there by name.
 */
internal class DiscImage(private val channel: FileChannel) {
    private val base: Long
    private val root: ByteBuffer

    init {
        // Where the game's partition starts in the kinds of image there
        // are; an image shorter than one of them is simply not that kind.
        base = longArrayOf(0x0, 0xFD90000, 0x2080000, 0x18300000, 0x18310000).firstOrNull {
            try {
                read(it + 32 * SECTOR, MAGIC.size).array().contentEquals(MAGIC)
            } catch (_: IOException) {
                false
            }
        } ?: throw IOException("this is not an Xbox 360 disc image")
        val volume = read(base + 32 * SECTOR + MAGIC.size, 8)
        val sector = volume.int.toLong() and 0xFFFFFFFFL
        val size = volume.int
        if (size <= 0 || size > (1 shl 24)) throw IOException("the disc image's root folder is damaged")
        root = read(base + sector * SECTOR, size)
    }

    private fun read(offset: Long, size: Int): ByteBuffer {
        val buffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        while (buffer.hasRemaining()) {
            if (channel.read(buffer, offset + buffer.position()) <= 0) throw IOException("the disc image is cut short")
        }
        buffer.flip()
        return buffer
    }

    /** The file's place in the image and its size, or null when the root folder has none of that name. */
    fun find(name: String): Pair<Long, Long>? {
        // The folder is a binary tree of entries, each at four times its
        // number; walked whole, so its order need not be trusted.
        val pending = ArrayDeque(listOf(0))
        val seen = HashSet<Int>()
        while (pending.isNotEmpty()) {
            val at = pending.removeLast() * 4
            if (!seen.add(at) || at + 14 > root.limit()) continue
            val left = root.getShort(at).toInt() and 0xFFFF
            val right = root.getShort(at + 2).toInt() and 0xFFFF
            if (left == 0xFFFF) continue
            val length = root.get(at + 13).toInt() and 0xFF
            if (at + 14 + length > root.limit()) continue
            if (left != 0) pending.addLast(left)
            if (right != 0) pending.addLast(right)
            val directory = root.get(at + 12).toInt() and 0x10 != 0
            val entry = String(root.array(), at + 14, length, Charsets.ISO_8859_1)
            if (!directory && entry.equals(name, ignoreCase = true)) {
                val sector = root.getInt(at + 4).toLong() and 0xFFFFFFFFL
                val size = root.getInt(at + 8).toLong() and 0xFFFFFFFFL
                return Pair(base + sector * SECTOR, size)
            }
        }
        return null
    }

    companion object {
        const val SECTOR = 2048L
        val MAGIC = "MICROSOFT*XBOX*MEDIA".toByteArray()
    }
}
