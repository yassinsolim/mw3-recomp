package com.mw2.recomp

import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.RandomAccessFile

/**
 * Why a disc image was not taken, in the kinds a player acts on
 * differently. Each is an [IOException], so what reads an image or its
 * executables throws them as it always threw its errors, and what asks the
 * player (SetupActivity, BuildActivity) tells them apart by type.
 *
 *  - [Unreadable]: the image cannot be read at all, or no longer to its end.
 *  - [NoSpace]: the executables could not be kept for want of space.
 *  - [NotThisGame]: what was read is no disc image of this game.
 *  - [UnsupportedVersion]: this game's disc, of a version this app has no
 *    game code for (another region, edition or pressing).
 *  - [Altered]: an executable of the supported version, but not the bytes of
 *    the disc's: a copy that is damaged or was changed.
 *  - [Failed]: anything else, with its own message.
 */
sealed class DiscRefusal(message: String, cause: Throwable? = null) : IOException(message, cause) {

    /** Why an image could not be read. */
    enum class Reason {
        /** Moved, deleted, its memory card out: nothing is there. */
        MISSING,

        /** Something is there, and the permission to read it is gone (a document's grant taken back, a folder the app may no longer list). */
        PERMISSION,

        /** A document that is no file on this device (a cloud entry): it cannot be read at positions. */
        NOT_SEEKABLE,

        /** Anything the system reports while reading: a failing card, a dropped drive. */
        IO,
    }

    class Unreadable(val reason: Reason, cause: Throwable) :
        DiscRefusal("the disc image cannot be read (${reason.name.lowercase()})", cause)

    class NoSpace(val needed: Long, cause: Throwable) :
        DiscRefusal("not enough free space to keep the executables: $needed bytes are needed", cause)

    class NotThisGame(message: String) : DiscRefusal(message)

    /** [found] is the executable's version number, [supported] those this app has the game code for (none known: empty). */
    class UnsupportedVersion(val executable: String, val found: Long, val supported: List<Long>) :
        DiscRefusal("$executable is version $found, which this app does not support" + supportedText(supported))

    class Altered(val executable: String, val version: Long) :
        DiscRefusal("$executable is version $version, as supported, but is not the disc's own executable")

    class Failed(cause: Throwable) : DiscRefusal(cause.message ?: cause.toString(), cause)

    companion object {
        private fun supportedText(versions: List<Long>) = if (versions.isEmpty()) "" else " (supported: ${versions.joinToString(", ")})"

        /** Every message along the chain of causes, for telling what the system reported. */
        private fun messages(error: Throwable): String = generateSequence(error) { it.cause?.takeIf { cause -> cause !== it } }
            .take(8).joinToString("\n") { it.message.orEmpty() }

        /** The kind of unreadable that what the system threw while opening or reading an image says. */
        fun unreadable(error: Throwable): Unreadable {
            val said = messages(error)
            val reason = when {
                error is SecurityException || "EACCES" in said || "EPERM" in said || "Permission denied" in said -> Reason.PERMISSION
                "ESPIPE" in said || "Illegal seek" in said -> Reason.NOT_SEEKABLE
                error is FileNotFoundException || "ENOENT" in said -> Reason.MISSING
                else -> Reason.IO
            }
            return Unreadable(reason, error)
        }

        /** Whether what the system threw while writing says the storage is full. */
        fun isNoSpace(error: Throwable): Boolean =
            messages(error).let { "ENOSPC" in it || "No space left" in it || "EDQUOT" in it }

        /** [error] and its causes, each as its class and message, as the setup log records it. */
        fun describe(error: Throwable): String = generateSequence(error) { it.cause?.takeIf { cause -> cause !== it } }
            .take(8).joinToString(" <- ") { "${it.javaClass.name}: ${it.message}" }
    }
}

/** What an executable's header says of it. */
internal object XexHeader {
    /** The key of the optional header that holds the execution info: { media id, version, base version, title id }. */
    private const val EXECUTION_INFO = 0x00040006

    /** The version number the header of [file] gives, or null when it is no Xbox 360 executable (tools/xexinfo.py, the runtime's install/xex.cpp). */
    fun version(file: File): Long? = try {
        RandomAccessFile(file, "r").use { input ->
            fun bytes(at: Long, size: Int): ByteArray? {
                if (at < 0 || at + size > input.length()) return null
                return ByteArray(size).also { input.seek(at); input.readFully(it) }
            }

            fun word(data: ByteArray, at: Int): Long =
                ((data[at].toLong() and 0xFF) shl 24) or ((data[at + 1].toLong() and 0xFF) shl 16) or
                    ((data[at + 2].toLong() and 0xFF) shl 8) or (data[at + 3].toLong() and 0xFF)

            val head = bytes(0, 24) ?: return null
            val count = word(head, 20)
            if (word(head, 0) != 0x58455832L || count !in 1..64) return null
            val entries = bytes(24, (count * 8).toInt()) ?: return null
            for (index in 0 until count.toInt()) {
                if (word(entries, index * 8) != EXECUTION_INFO.toLong()) continue
                val info = bytes(word(entries, index * 8 + 4), 8) ?: return null
                return word(info, 4)
            }
            null
        }
    } catch (_: IOException) {
        null
    }
}
