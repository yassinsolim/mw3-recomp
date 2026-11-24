package com.mw2.recomp

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * A disc image of the player's own copy of the game.
 *
 * An instance is the image read: the root folder of an Xbox 360 disc image
 * (XDVDFS), as tools/xdvdfs.py and the runtime's installer (launcher/disc.cpp)
 * read it, enough to find a file there by name.
 *
 * The companion is which image and how it is opened, for a game played from
 * its image without its files copied into the app (runtime/kernel/volume.h
 * reads them out of the image where it lies). The image is kept as text
 * (Prefs.discImage): a path, for an image the app can read as a file -- in
 * its own folders, or anywhere once the player has allowed all-files access
 * -- or a document's address, for one chosen with the system's picker. A
 * document has no path; the app keeps the permission the picker gave (it
 * lasts across restarts) and hands the runtime the open file instead, as
 * "fd:<number>".
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

        fun isDocument(image: String): Boolean = image.startsWith("content://")

        /**
         * What the runtime is given to open: the path, or "fd:<number>" of the
         * document opened here. Null when the image cannot be read any more: it
         * was deleted or moved, its memory card is out, or the permission was
         * taken back. The descriptor stays open until [close] or the process's
         * end; the runtime reads through it for the whole run.
         */
        fun source(context: Context, image: String): String? {
            if (!isDocument(image)) return image.takeIf { File(it).let { file -> file.isFile && file.canRead() } }
            val opened = try {
                context.contentResolver.openFileDescriptor(Uri.parse(image), "r")
            } catch (_: Throwable) {
                null
            } ?: return null
            // The runtime reads at positions, which a file can do and a pipe from
            // a cloud provider cannot.
            val seekable = try {
                OsConstants.S_ISREG(Os.fstat(opened.fileDescriptor).st_mode)
            } catch (_: Throwable) {
                false
            }
            if (!seekable) {
                opened.close()
                return null
            }
            return "fd:${opened.detachFd()}"
        }

        /** Closes the document [source] opened, once nothing reads it. */
        fun close(source: String?) {
            val descriptor = source?.removePrefix("fd:")?.takeIf { it != source }?.toIntOrNull() ?: return
            try {
                ParcelFileDescriptor.adoptFd(descriptor).close()
            } catch (_: Throwable) {
            }
        }

        /** Keeps the picker's permission to read [uri] across restarts. */
        fun keep(context: Context, uri: Uri) {
            try {
                context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: Throwable) {
                // A provider that grants none for keeps: the image reads until
                // the app is restarted, and the launcher then says it is gone.
            }
        }

        /** The image as a player would name it: its path, or the document's name. */
        fun label(context: Context, image: String): String {
            if (!isDocument(image)) return image
            try {
                context.contentResolver.query(Uri.parse(image), null, null, null, null)?.use { cursor ->
                    val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (column >= 0 && cursor.moveToFirst()) cursor.getString(column)?.let { return it }
                }
            } catch (_: Throwable) {
            }
            return Uri.parse(image).lastPathSegment ?: image
        }

        /**
         * Disc images where players leave them, on the device's own storage and
         * on each memory card: the Download folder's, largest first. Empty
         * without the permission to list those folders.
         */
        fun inDownloads(context: Context): List<File> {
            val roots = mutableListOf(File("/sdcard"))
            // A card's root, from the app's own folder on it
            // (<root>/Android/data/<package>/files).
            for (folder in context.getExternalFilesDirs(null).drop(1)) {
                val root = folder?.absolutePath?.substringBefore("/Android/data/", "") ?: continue
                if (root.isNotEmpty()) roots += File(root)
            }
            return roots.flatMap { root ->
                File(root, "Download").listFiles { file ->
                    file.isFile && file.name.endsWith(".iso", ignoreCase = true) && file.length() > SMALLEST
                }?.toList() ?: emptyList()
            }.sortedByDescending { it.length() }
        }

        /** No disc of these games is smaller; a file that is, is something else. */
        const val SMALLEST = 500_000_000L
    }
}
