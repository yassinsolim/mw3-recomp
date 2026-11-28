package com.mw2.recomp

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.system.Os
import android.system.OsConstants
import android.os.Environment
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
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
            } catch (_: DiscRefusal.NotThisGame) {
                false
            }
        } ?: throw DiscRefusal.NotThisGame("this is not an Xbox 360 disc image")
        val volume = read(base + 32 * SECTOR + MAGIC.size, 8)
        val sector = volume.int.toLong() and 0xFFFFFFFFL
        val size = volume.int
        if (size <= 0 || size > (1 shl 24)) throw DiscRefusal.NotThisGame("the disc image's root folder is damaged")
        root = read(base + sector * SECTOR, size)
    }

    private fun read(offset: Long, size: Int): ByteBuffer {
        val buffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        while (buffer.hasRemaining()) {
            val read = try {
                channel.read(buffer, offset + buffer.position())
            } catch (error: IOException) {
                // What the system says: a card that failed, a file taken away, a permission taken back.
                throw DiscRefusal.unreadable(error)
            }
            if (read <= 0) throw DiscRefusal.NotThisGame("the disc image is cut short")
        }
        buffer.flip()
        return buffer
    }

    /** What a disc image is to this app. */
    enum class Kind {
        /** This game's game disc: the one with its executables. */
        GAME,

        /** Advanced Warfare's install disc: the one with its two content packages. */
        INSTALL,

        /** A disc of something else. */
        OTHER,
    }

    /** This game's disc images among some files: its game discs and, for Advanced Warfare, its install discs. */
    class Found(val games: List<File>, val installs: List<File>)

    /**
     * The entry [name] of the folder [table]: where it starts in the image
     * and its size, of a file or (with [directory]) of a folder.
     */
    private fun entry(table: ByteBuffer, name: String, directory: Boolean): Pair<Long, Long>? {
        // The folder is a binary tree of entries, each at four times its
        // number; walked whole, so its order need not be trusted.
        val pending = ArrayDeque(listOf(0))
        val seen = HashSet<Int>()
        while (pending.isNotEmpty()) {
            val at = pending.removeLast() * 4
            if (!seen.add(at) || at + 14 > table.limit()) continue
            val left = table.getShort(at).toInt() and 0xFFFF
            val right = table.getShort(at + 2).toInt() and 0xFFFF
            if (left == 0xFFFF) continue
            val length = table.get(at + 13).toInt() and 0xFF
            if (at + 14 + length > table.limit()) continue
            if (left != 0) pending.addLast(left)
            if (right != 0) pending.addLast(right)
            val folder = table.get(at + 12).toInt() and 0x10 != 0
            val entry = String(table.array(), at + 14, length, Charsets.ISO_8859_1)
            if (folder == directory && entry.equals(name, ignoreCase = true)) {
                val sector = table.getInt(at + 4).toLong() and 0xFFFFFFFFL
                val size = table.getInt(at + 8).toLong() and 0xFFFFFFFFL
                return Pair(base + sector * SECTOR, size)
            }
        }
        return null
    }

    /** The folder at [path] below the root ("" is the root), or null when the image has none. */
    private fun folder(path: String): ByteBuffer? {
        var table = root
        for (part in path.split('/').filter { it.isNotEmpty() }) {
            val (at, size) = entry(table, part, directory = true) ?: return null
            if (size <= 0 || size > (1 shl 24)) return null
            table = try {
                read(at, size.toInt())
            } catch (_: DiscRefusal.NotThisGame) {
                return null
            }
        }
        return table
    }

    /**
     * The file's place in the image and its size, or null when the image has
     * none of that name: one at the root, or by its path ("<folder>/<file>").
     */
    fun find(name: String): Pair<Long, Long>? =
        folder(name.substringBeforeLast('/', ""))?.let { entry(it, name.substringAfterLast('/'), directory = false) }

    /** The number of the title the executable [name] belongs to, from its header (tools/xexinfo.py), or null. */
    private fun titleOf(name: String): Long? {
        val (at, size) = find(name) ?: return null
        return try {
            // { "XEX2", flags, the image's offset, reserved, the security
            //   info's offset, the count of optional headers } and then those,
            //   { key, value } each; the execution info's value is where it is.
            val header = read(at, 24).order(ByteOrder.BIG_ENDIAN)
            val count = header.getInt(20)
            if (header.getInt(0) != 0x58455832 || count !in 1..64 || 24 + count * 8L > size) return null
            val entries = read(at + 24, count * 8).order(ByteOrder.BIG_ENDIAN)
            for (index in 0 until count) {
                val where = entries.getInt(index * 8 + 4).toLong() and 0xFFFFFFFFL
                if (entries.getInt(index * 8) != EXECUTION_INFO || where + 16 > size) continue
                return read(at + where + 12, 4).order(ByteOrder.BIG_ENDIAN).int.toLong() and 0xFFFFFFFFL
            }
            null
        } catch (_: DiscRefusal.NotThisGame) {
            null
        }
    }

    /**
     * What this image is to this app. An install disc is told first, by the
     * folder its packages are in: Advanced Warfare's has the campaign's
     * executable at its root as well.
     */
    fun kind(): Kind = when {
        Title.isAw && folder("content/0000000000000000/%08X/00000002".format(Title.titleId)) != null -> Kind.INSTALL
        Title.carried.all { find(it.executable) != null } && titleOf(Title.carried.first().executable) == Title.titleId -> Kind.GAME
        else -> Kind.OTHER
    }

    /**
     * The number of the game this disc is, from its campaign's executable
     * (every one of these games has one at the root), or null: what the
     * guided setup names when the player picked another game's disc
     * (SetupActivity).
     */
    fun game(): Long? = titleOf(Title.CAMPAIGN.executable)

    companion object {
        const val SECTOR = 2048L
        val MAGIC = "MICROSOFT*XBOX*MEDIA".toByteArray()

        /** The key of an executable's optional header that holds its title's number. */
        private const val EXECUTION_INFO = 0x00040006

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

        /** The device's storage and each memory card's, by their roots. */
        fun storages(context: Context): List<File> = buildList {
            add(Environment.getExternalStorageDirectory())
            // A card's root, from the app's own folder on it
            // (<root>/Android/data/<package>/files).
            for (own in context.getExternalFilesDirs(null).drop(1)) {
                val root = own?.absolutePath?.substringBefore("/Android/data/", "") ?: continue
                if (root.isNotEmpty()) add(File(root))
            }
        }

        /**
         * The image opened for reading at positions: its path, or a document's
         * address. Throws a [DiscRefusal.Unreadable] that says whether it is
         * gone or its permission is.
         */
        fun open(context: Context, image: String): FileChannel = try {
            if (isDocument(image)) {
                // A provider that answers with no file: the entry is gone.
                val descriptor = context.contentResolver.openFileDescriptor(Uri.parse(image), "r")
                    ?: throw FileNotFoundException("the provider has no file for $image")
                ParcelFileDescriptor.AutoCloseInputStream(descriptor).channel
            } else {
                FileInputStream(image).channel
            }
        } catch (error: Exception) {
            throw DiscRefusal.unreadable(error)
        }

        /** [what] of [image], a path or a document's address, read as a disc image; null when it is none or cannot be read. */
        private fun <T> opened(context: Context, image: String, what: (DiscImage) -> T): T? = try {
            open(context, image).use { what(DiscImage(it)) }
        } catch (_: Throwable) {
            // Not readable, not an image, or a provider that took the
            // permission back: all the same to the one asking.
            null
        }

        /**
         * What [image], a path or a document's address, is to this app; null
         * when it is not an Xbox 360 disc image or cannot be read.
         */
        fun kindOf(context: Context, image: String): Kind? = opened(context, image) { it.kind() }

        /**
         * What [image] is to this app and the number of the game it is a disc
         * of. Throws a [DiscRefusal] that says why it could not be read: gone,
         * unreadable, or no disc image.
         */
        fun inspect(context: Context, image: String): Pair<Kind, Long?> = try {
            open(context, image).use { channel -> DiscImage(channel).let { Pair(it.kind(), it.game()) } }
        } catch (refusal: DiscRefusal) {
            throw refusal
        } catch (error: Exception) {
            throw DiscRefusal.Failed(error)
        }

        private fun isImageFile(file: File): Boolean =
            file.name.endsWith(".iso", ignoreCase = true) && file.isFile && file.length() > SMALLEST

        private fun sorted(context: Context, images: List<File>): Found {
            val kinds = images.associateWith { kindOf(context, it.path) }
            return Found(images.filter { kinds[it] == Kind.GAME }, images.filter { kinds[it] == Kind.INSTALL })
        }

        /** The disc images of this game lying in [folder] itself: what a player means who chooses the folder they are in. */
        fun inFolder(context: Context, folder: File): Found =
            sorted(context, folder.listFiles { file -> isImageFile(file) }.orEmpty().sortedBy { it.name })

        /**
         * The disc images of this game on the device's own storage and on
         * each memory card, wherever players keep them: the Download folders
         * first, then every folder down to [DEEPEST] below a storage's root
         * (ROMs/xbox 360/<game>/ is three), nearer ones first. Each image is
         * told by what is in it, not by its name. Empty without the
         * permission to list those folders. It reads folders and so is not
         * for the main thread.
         */
        fun search(context: Context): Found {
            val images = ArrayList<File>()
            var level = storages(context)
            var visited = 0
            for (depth in 0..DEEPEST) {
                val next = ArrayList<File>()
                for (folder in level) {
                    // A storage of very many folders is not walked whole.
                    if (++visited > MOST_FOLDERS) break
                    for (entry in folder.listFiles().orEmpty()) {
                        if (entry.name.startsWith(".")) continue
                        if (isImageFile(entry)) images += entry
                        // Android/ is other apps' folders, which no app may read.
                        else if (entry.isDirectory && !(depth == 0 && entry.name == "Android")) next += entry
                    }
                }
                level = next
            }
            // Stable: within Download and within the rest, the nearer first.
            return sorted(context, images.sortedBy { it.parentFile?.name != Environment.DIRECTORY_DOWNLOADS })
        }

        /** No disc of these games is smaller; a file that is, is something else. */
        const val SMALLEST = 500_000_000L
        private const val DEEPEST = 4
        private const val MOST_FOLDERS = 3000
    }
}
