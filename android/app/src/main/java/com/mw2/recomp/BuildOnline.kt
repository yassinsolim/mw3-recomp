package com.mw2.recomp

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

/**
 * For a player with no PC to build the game code on (GameLibrary.kt):
 * the disc's two executables, written into the device's Download folder
 * under names that say the game (mw2-default.xex, mw2-default_mp.xex). They
 * are all a build needs of the disc, a few megabytes each, and the player
 * uploads them to a private repository of their own, where GitHub builds the
 * game package from them (docs/BUILD-WITHOUT-PC.md,
 * setup/recomp-builder). Nothing leaves the device here.
 */
object BuildOnline {
    /**
     * Only the published app has its game code built elsewhere, and a repository that says how.
     * Not Advanced Warfare's: setup/recomp-builder does not build that game.
     */
    val offered: Boolean = GameLibrary.separate && BuildConfig.UPDATE_REPO.endsWith("-recomp") && !Title.isAw

    /** mw3, mw2 or cod4: what the build calls the game, from the repository the app is published in. */
    private val game: String = BuildConfig.UPDATE_REPO.substringAfter('/').removeSuffix("-recomp")

    val guide: Uri = Uri.parse("https://github.com/${BuildConfig.UPDATE_REPO}/blob/main/docs/BUILD-WITHOUT-PC.md")

    /** The name [title]'s executable has in Download. */
    fun fileName(title: Title): String = "$game-${title.executable}"

    /** Whether the installed game files in [gameDirectory] have both executables, so no disc image is needed. */
    fun inGameFolder(gameDirectory: String): Boolean = Title.carried.all { it.installed(gameDirectory) }

    /** From the installed game files. Throws an [IOException] that says what went wrong. */
    fun fromGameFolder(context: Context, gameDirectory: String) {
        for (title in Title.carried) {
            val file = File(gameDirectory, title.executable)
            FileInputStream(file).channel.use { write(context, title, it, 0, file.length()) }
        }
    }

    /** Out of the disc image the player picked, which is only read. Throws an [IOException] that says what went wrong. */
    fun fromDiscImage(context: Context, image: Uri) {
        val descriptor = context.contentResolver.openFileDescriptor(image, "r")
            ?: throw IOException("the file could not be opened")
        descriptor.use {
            FileInputStream(it.fileDescriptor).channel.use { channel ->
                val root = DiscImage(channel)
                for (title in Title.carried) {
                    val (offset, size) = root.find(title.executable)
                        ?: throw IOException("${title.executable} is not on that disc image")
                    write(context, title, channel, offset, size)
                }
            }
        }
    }

    private fun write(context: Context, title: Title, source: FileChannel, offset: Long, size: Long) {
        val magic = ByteBuffer.allocate(4)
        source.read(magic, offset)
        if (size < 4 || !magic.array().contentEquals("XEX2".toByteArray())) {
            throw IOException("${title.executable} is not an Xbox 360 executable")
        }
        download(context, fileName(title)) { output ->
            val buffer = ByteBuffer.allocate(1 shl 20)
            var done = 0L
            while (done < size) {
                buffer.clear().limit(minOf(buffer.capacity().toLong(), size - done).toInt())
                val read = source.read(buffer, offset + done)
                if (read <= 0) throw IOException("the disc image ends inside ${title.executable}")
                output.write(buffer.array(), 0, read)
                done += read
            }
        }
    }

    /**
     * A file in the shared Download folder, through the media store: it
     * needs no permission, and a browser's upload dialog opens there.
     */
    private fun download(context: Context, name: String, write: (OutputStream) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) throw IOException("this needs Android 10 or later")
        val resolver = context.contentResolver
        val downloads = MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        // The one written before: left there, the new one would be "name (1)".
        resolver.delete(downloads, "${MediaStore.MediaColumns.DISPLAY_NAME} = ?", arrayOf(name))
        val entry = resolver.insert(downloads, ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, name)
            put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream")
            put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
        }) ?: throw IOException("the Download folder cannot be written")
        try {
            (resolver.openOutputStream(entry) ?: throw IOException("the Download folder cannot be written")).use(write)
        } catch (failure: Throwable) {
            resolver.delete(entry, null, null)
            throw failure
        }
    }
}
