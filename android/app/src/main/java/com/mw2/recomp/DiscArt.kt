package com.mw2.recomp

import android.content.Context
import java.io.File

/**
 * The game's own artwork for the launcher, from the player's disc.
 *
 * Every disc carries `nxeart` at its root: the package the console's
 * dashboard reads the game's backdrop from (`nxebg.jpg`, 1920x1080). The
 * installer copies it with the game files (runtime/install/install.cpp), so
 * it is on the phone of anyone who has installed the game and nowhere else:
 * the app is published without a pixel of it. Here the picture is taken out
 * of the package once and kept in the app's private folder, so the launcher
 * decodes a plain JPEG each time it opens. It is kept as the JPEG it is:
 * written again as a PNG it would be several times the size and no sharper.
 */
object DiscArt {
    private const val PACKAGE = "nxeart"
    private const val BACKDROP = "nxebg.jpg"

    /** The real ones are about a megabyte; anything far larger is not one. */
    private const val LARGEST_PACKAGE = 16L * 1024 * 1024

    /**
     * The backdrop of the game in [gameDirectory] as a file to decode, or
     * null when the game is not installed or its package cannot be read. It
     * reads and writes files: not for the main thread.
     */
    fun backdrop(context: Context, gameDirectory: File): File? {
        val folder = File(context.filesDir, "art")
        val cached = File(folder, BACKDROP)
        val stampFile = File(folder, "$BACKDROP.from")
        val source = File(gameDirectory, PACKAGE)
        try {
            if (!source.isFile || source.length() > LARGEST_PACKAGE) {
                // The game was removed: its picture goes with it.
                cached.delete()
                stampFile.delete()
                return null
            }
            // Which package the kept picture came from: another disc installed
            // over this one has another size or time.
            val stamp = "${source.length()}:${source.lastModified()}"
            if (cached.isFile && stampFile.isFile && stampFile.readText() == stamp) return cached
            val picture = StfsPackage.file(source.readBytes(), BACKDROP) ?: return null
            folder.mkdirs()
            // Under another name until it is whole: a write cut short must not
            // be taken for the picture the next time.
            val partial = File(folder, "$BACKDROP.part")
            partial.writeBytes(picture)
            if (!partial.renameTo(cached)) return null
            stampFile.writeText(stamp)
            return cached
        } catch (_: Throwable) {
            // Unreadable storage: the launcher keeps its own backdrop.
            return null
        }
    }
}

/**
 * Reading one file out of an Xbox 360 content package of the read-only kind
 * (LIVE and PIRS, one hash table per group of 170 blocks), which is what a
 * disc's `nxeart` is. The layout is tools/stfs.py's.
 */
object StfsPackage {
    private const val BLOCK = 0x1000
    private const val PER_TABLE = 0xAA          // data blocks one hash table covers
    private const val PER_LEVEL1 = 0x70E4       // and one second-level table
    private const val FIRST_BLOCK = 0xC000
    private const val ENTRY = 0x40

    /** The contents of [name] at the package's root, or null: not such a package, no such file, or cut short. */
    fun file(data: ByteArray, name: String): ByteArray? {
        if (data.size < FIRST_BLOCK) return null
        val magic = String(data, 0, 4, Charsets.ISO_8859_1)
        if (magic != "LIVE" && magic != "PIRS") return null
        // A console-signed package keeps two hash tables per group and its
        // blocks are elsewhere; a disc never carries one.
        val headerSize = big(data, 0x340, 4)
        if (((headerSize + 0xFFF) and 0xF000) shr 12 != 0xB) return null

        val table = chain(data, little(data, 0x37E, 3), little(data, 0x37C, 2), Int.MAX_VALUE) ?: return null
        var at = 0
        while (at + ENTRY <= table.size && table[at].toInt() != 0) {
            val flags = table[at + 0x28].toInt() and 0xFF
            val folder = flags and 0x80 != 0
            val parent = big(table, at + 0x32, 2).toShort().toInt()
            if (!folder && parent < 0 && String(table, at, flags and 0x3F, Charsets.ISO_8859_1) == name) {
                return chain(data, little(table, at + 0x2F, 3), little(table, at + 0x29, 3), big(table, at + 0x34, 4))
            }
            at += ENTRY
        }
        return null
    }

    /** Where a data block is in the file: the hash tables sit between them. */
    private fun offset(block: Int): Long {
        var tables = 0
        if (block >= PER_TABLE) tables += block / PER_TABLE + 1
        if (block >= PER_LEVEL1) tables += block / PER_LEVEL1 + 1
        return FIRST_BLOCK + (block + tables).toLong() * BLOCK
    }

    /**
     * [count] blocks from [start], each naming the next in its group's hash
     * table (the block before the group's first), cut to [size] bytes.
     */
    private fun chain(data: ByteArray, start: Int, count: Int, size: Int): ByteArray? {
        if (count < 0 || size < 0 || count.toLong() * BLOCK > data.size) return null
        val out = ByteArray(minOf(count * BLOCK, size))
        var block = start
        for (i in 0 until count) {
            val from = offset(block)
            if (from + BLOCK > data.size) return null
            val wanted = minOf(BLOCK, out.size - i * BLOCK)
            if (wanted > 0) System.arraycopy(data, from.toInt(), out, i * BLOCK, wanted)
            val entry = offset(block - block % PER_TABLE) - BLOCK + (block % PER_TABLE) * 0x18
            if (entry + 0x18 > data.size) return null
            block = big(data, (entry + 0x15).toInt(), 3)
        }
        return out
    }

    private fun big(data: ByteArray, at: Int, bytes: Int): Int {
        var value = 0
        for (i in 0 until bytes) value = (value shl 8) or (data[at + i].toInt() and 0xFF)
        return value
    }

    private fun little(data: ByteArray, at: Int, bytes: Int): Int {
        var value = 0
        for (i in bytes - 1 downTo 0) value = (value shl 8) or (data[at + i].toInt() and 0xFF)
        return value
    }
}
