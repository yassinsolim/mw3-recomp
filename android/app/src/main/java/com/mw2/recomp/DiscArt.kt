package com.mw2.recomp

import android.content.Context
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

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
 * Reading an Xbox 360 content package of the read-only kind (LIVE and PIRS,
 * one hash table per group of 170 blocks): one file out of a disc's
 * `nxeart`, and the names at the root of a map pack, which say what the
 * pack is (Dlc.kt). The layout is tools/stfs.py's.
 */
object StfsPackage {
    private const val BLOCK = 0x1000
    private const val PER_TABLE = 0xAA          // data blocks one hash table covers
    private const val PER_LEVEL1 = 0x70E4       // and one second-level table
    private const val FIRST_BLOCK = 0xC000
    private const val ENTRY = 0x40
    private const val HEADER = 0x381            // through the file table's first block

    /**
     * A package's bytes wherever they are: a disc's `nxeart` whole in memory,
     * a map pack of a gigabyte in its file and read only where it is asked.
     */
    private abstract class Bytes(val size: Long) {
        /** [length] bytes from [at], which the caller has checked are within [size]. */
        abstract fun read(at: Long, into: ByteArray, offset: Int, length: Int)
    }

    /** The contents of [name] at the package's root, or null: not such a package, no such file, or cut short. */
    fun file(data: ByteArray, name: String): ByteArray? {
        val bytes = object : Bytes(data.size.toLong()) {
            override fun read(at: Long, into: ByteArray, offset: Int, length: Int) =
                System.arraycopy(data, at.toInt(), into, offset, length)
        }
        val table = table(bytes) ?: return null
        eachRootFile(table) { entry, found ->
            if (found == name) {
                return chain(bytes, little(table, entry + 0x2F, 3), little(table, entry + 0x29, 3), big(table, entry + 0x34, 4))
            }
        }
        return null
    }

    /** The names of the files at the root of the package in [file]; none when it is not such a package or cannot be read. */
    fun rootFiles(file: File): List<String> = try {
        RandomAccessFile(file, "r").use { input ->
            val bytes = object : Bytes(input.length()) {
                override fun read(at: Long, into: ByteArray, offset: Int, length: Int) {
                    input.seek(at)
                    input.readFully(into, offset, length)
                }
            }
            val names = ArrayList<String>()
            table(bytes)?.let { table -> eachRootFile(table) { _, name -> names.add(name) } }
            names
        }
    } catch (_: IOException) {
        emptyList()
    }

    /** The package's file table, or null: not a read-only package, or cut short. */
    private fun table(bytes: Bytes): ByteArray? {
        if (bytes.size < FIRST_BLOCK) return null
        val head = ByteArray(HEADER)
        bytes.read(0, head, 0, head.size)
        val magic = String(head, 0, 4, Charsets.ISO_8859_1)
        if (magic != "LIVE" && magic != "PIRS") return null
        // A console-signed package keeps two hash tables per group and its
        // blocks are elsewhere; neither a disc nor the marketplace has one.
        val headerSize = big(head, 0x340, 4)
        if (((headerSize + 0xFFF) and 0xF000) shr 12 != 0xB) return null
        return chain(bytes, little(head, 0x37E, 3), little(head, 0x37C, 2), Int.MAX_VALUE)
    }

    /** Each file at the root of the file table [table]: where its entry is, and its name. */
    private inline fun eachRootFile(table: ByteArray, each: (entry: Int, name: String) -> Unit) {
        var at = 0
        while (at + ENTRY <= table.size && table[at].toInt() != 0) {
            val flags = table[at + 0x28].toInt() and 0xFF
            val folder = flags and 0x80 != 0
            val parent = big(table, at + 0x32, 2).toShort().toInt()
            if (!folder && parent < 0) each(at, String(table, at, flags and 0x3F, Charsets.ISO_8859_1))
            at += ENTRY
        }
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
    private fun chain(bytes: Bytes, start: Int, count: Int, size: Int): ByteArray? {
        if (count < 0 || size < 0 || count.toLong() * BLOCK > bytes.size) return null
        val out = ByteArray(minOf(count * BLOCK, size))
        val next = ByteArray(3)
        var block = start
        for (i in 0 until count) {
            val from = offset(block)
            if (from + BLOCK > bytes.size) return null
            val wanted = minOf(BLOCK, out.size - i * BLOCK)
            if (wanted > 0) bytes.read(from, out, i * BLOCK, wanted)
            val entry = offset(block - block % PER_TABLE) - BLOCK + (block % PER_TABLE) * 0x18
            if (entry + 0x18 > bytes.size) return null
            bytes.read(entry + 0x15, next, 0, next.size)
            block = big(next, 0, 3)
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
