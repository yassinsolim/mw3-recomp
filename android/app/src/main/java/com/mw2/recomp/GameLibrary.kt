package com.mw2.recomp

import android.content.Context
import android.content.pm.PackageManager
import java.io.File
import java.io.IOException

/**
 * The recompiled game code, where the app is published without it
 * (BuildConfig.GAME_PACKAGE; docs/game-library.md). It is made from the
 * player's own disc, in one of two places, and the runtime loads the title's
 * library from whichever has it:
 *
 *  - on this device, by the app itself (GameBuild.kt, docs/ondevice-build.md):
 *    a file in the app's own storage, [built];
 *  - on a PC (setup\setup.cmd), installed as a package of its own with no
 *    code and no screen, whose library folder Android unpacks; this app asks
 *    the system where that is.
 *
 * An update of the app leaves both alone, and the runtime refuses a library
 * that is not the code this version of the app expects
 * (runtime/android/game_library.cpp). Whether it will is known here before
 * a run, from the statement each library carries ([taken]): that is what
 * starts the game code's update by itself (GameUpdate.kt).
 */
object GameLibrary {
    /** Whether the game code is apart from the app's own libraries in this build. */
    val separate: Boolean = BuildConfig.GAME_PACKAGE.isNotEmpty()

    /**
     * Where a build on this device leaves a library: the app's private
     * storage, which only the app can write, and from which Android lets an
     * app load code it wrote (seen on Android 13 with targetSdk 35: it is
     * running a program from there that is refused, not mapping a library).
     */
    fun built(context: Context, library: String): File = File(File(context.filesDir, "game-code"), "lib${library}game.so")

    fun built(context: Context, title: Title): File = built(context, title.library)

    /** Beside a library built here: its statement, to tell without opening it whether this app takes it. */
    fun statementOf(library: File): File = File(library.path + ".abi")

    /** A library of the game package, or null when it is not installed. */
    private fun packaged(context: Context, library: String): File? {
        val folder = try {
            context.packageManager.getApplicationInfo(BuildConfig.GAME_PACKAGE, 0).nativeLibraryDir
        } catch (_: PackageManager.NameNotFoundException) {
            return null
        }
        return File(folder, "lib${library}game.so").takeIf { it.isFile }
    }

    /** The title's library in the game package, or null when it is not installed. */
    private fun packaged(context: Context, title: Title): File? = packaged(context, title.library)

    /**
     * Whether the game code of the multiplayer as its title update makes it
     * is here (TitleUpdate): built on this device once the update's package
     * was added (GameBuild.kt), or in the game package from a PC's setup
     * with -Dlc.
     */
    fun updateReady(context: Context): Boolean = !separate || files(context, TitleUpdate.LIBRARY).isNotEmpty()

    /**
     * A library's files on this device, the one built here first: it is
     * what the player did last in this app, and a package from a PC that is
     * newer than it is still reached when the runtime refuses this one.
     */
    private fun files(context: Context, library: String): List<File> =
        listOfNotNull(built(context, library).takeIf { it.isFile }, packaged(context, library))

    /** The libraries a run of [title] loads. */
    private fun files(context: Context, title: Title): List<File> = files(context, title.runLibrary)

    /** Whether there is game code on this device at all: what the launcher says before a run. */
    fun installed(context: Context): Boolean =
        !separate || Title.carried.any { files(context, it).isNotEmpty() }

    /** Whether [title]'s game code is on this device: what a run of it needs. */
    fun ready(context: Context, title: Title): Boolean = !separate || files(context, title).isNotEmpty()

    // ---- what a library states, without loading it ------------------------------

    private fun stamp(file: File): String = "${file.path}|${file.length()}|${file.lastModified()}"

    /** What was read out of the game package's libraries, kept until the package is another. */
    private fun store(context: Context) = context.getSharedPreferences("game-code", Context.MODE_PRIVATE)

    /**
     * The statement in [file], a library of [library] (the text of its
     * mw2_game_abi, cmake/game_abi.cmake): looked for in the file's bytes by
     * its first line, since loading the library needs the runtime and a
     * process to lose. Null when the file has none.
     */
    private fun statementIn(file: File, library: String): String? {
        val first = "title $library\n".toByteArray(Charsets.ISO_8859_1)
        try {
            file.inputStream().use { input ->
                // A megabyte at a time, with the end of the last kept in
                // front: a statement is under two kilobytes and may lie
                // across two of them.
                val keep = 2048
                val buffer = ByteArray((1 shl 20) + keep)
                var held = 0
                while (true) {
                    val read = input.read(buffer, held, buffer.size - held)
                    val filled = held + maxOf(read, 0)
                    var at = 0
                    while (at + first.size <= filled) {
                        if (buffer[at] == first[0] && (1 until first.size).all { buffer[at + it] == first[it] }) {
                            var end = at
                            while (end < filled && buffer[end] != 0.toByte()) end++
                            // Not all of it is here yet: found again in the next piece.
                            if (end == filled && read > 0 && at >= filled - keep) break
                            val text = String(buffer, at, end - at, Charsets.ISO_8859_1)
                            if (STATEMENT.matches(text)) return text
                        }
                        at++
                    }
                    if (read <= 0) return null
                    held = minOf(keep, filled)
                    System.arraycopy(buffer, filled - held, buffer, 0, held)
                }
            }
        } catch (_: IOException) {
            return null
        }
    }

    /** What [file], one of [library]'s files, states; null when it cannot be told. */
    private fun statement(context: Context, library: String, file: File): String? {
        if (file == built(context, library)) {
            return try {
                statementOf(file).readText()
            } catch (_: IOException) {
                null
            }
        }
        // The package's library is tens of megabytes: read once for each
        // package installed. The statement is kept in front of the file's
        // stamp, since it ends in a new line, and a text that does comes
        // back from Android's preferences with spaces behind it.
        val key = "statement:$library"
        val kept = store(context).getString(key, null)
        val stamp = stamp(file)
        if (kept != null && kept.substringAfterLast('\n') == stamp) return kept.substringBeforeLast('\n').ifEmpty { null }?.plus("\n")
        val read = statementIn(file, library)
        store(context).edit().putString(key, read?.trimEnd('\n').orEmpty() + "\n" + stamp).apply()
        return read
    }

    /** The libraries the runtime refused although their statement is one this app takes, as [stamp]s. */
    private fun refusedFile(context: Context): File = File(File(context.filesDir, "game-code"), "refused")

    private fun refused(context: Context, file: File): Boolean = try {
        stamp(file) in refusedFile(context).readLines()
    } catch (_: IOException) {
        false
    }

    /**
     * Whether this version of the app takes [file], one of [library]'s
     * files, by what it states. An app with no build kit cannot tell: the
     * runtime says it when the library is loaded.
     */
    private fun taken(context: Context, library: String, file: File): Boolean {
        val expected = GameBuild.kit(context)?.titles?.firstOrNull { it.library == library } ?: return true
        return expected.takes(statement(context, library, file))
    }

    private fun taken(context: Context, title: Title, file: File): Boolean = taken(context, title.runLibrary, file)

    /** Whether [library] has a file on this device that this version of the app runs. */
    fun current(context: Context, library: String): Boolean =
        !separate || files(context, library).any { taken(context, library, it) && !refused(context, it) }

    /** Whether a run of [title] has a library this version of the app runs. */
    fun current(context: Context, title: Title): Boolean = current(context, title.runLibrary)

    /**
     * Whether [library]'s game code is here and was made for another version
     * of the app: every file of it states code this one does not take.
     */
    fun outdated(context: Context, library: String): Boolean =
        separate && files(context, library).let { files -> files.isNotEmpty() && files.none { taken(context, library, it) } }

    /** Where each title's library is, for a log (DeviceInfo). */
    fun describe(context: Context): String {
        if (!separate) return "part of this app"
        val version = try {
            context.packageManager.getPackageInfo(BuildConfig.GAME_PACKAGE, 0).versionName
        } catch (_: PackageManager.NameNotFoundException) {
            null
        }
        val packageSaid = if (version != null) "${BuildConfig.GAME_PACKAGE} $version" else "${BuildConfig.GAME_PACKAGE} is not installed"
        return "$packageSaid, " + Title.carried.joinToString {
            "lib${it.library}game.so " + when {
                built(context, it).isFile -> "was built on this device"
                packaged(context, it) != null -> "is in the package"
                else -> "is MISSING"
            }
        }
    }

    /**
     * Loads the title's game code into this process, after the runtime
     * (NativeBridge.load). Null when it is ready, or what to tell the player:
     * of the first library there is, since the second is only the fallback.
     * The libraries whose statement this app takes are tried first.
     */
    fun load(context: Context, title: Title): String? {
        if (!separate) return null
        var refusal: String? = null
        for (library in files(context, title).sortedBy { !taken(context, title, it) }) {
            val problem = try {
                NativeBridge.nativeLoadGame(library.absolutePath)
            } catch (error: Throwable) {
                error.message ?: error.toString()
            }
            if (problem.isEmpty()) return null
            // One the statement let through and the runtime did not (it
            // names something this runtime has not got): remembered, so that
            // the launcher offers the build instead of the same refusal.
            if (taken(context, title, library) && !refused(context, library)) {
                try {
                    refusedFile(context).apply { parentFile?.mkdirs() }.appendText(stamp(library) + "\n")
                } catch (_: IOException) {
                    // The refusal is still said below.
                }
            }
            if (refusal == null) {
                // An app that can build the code says so first: it is the
                // few minutes' way, and the dialog leads to it (GameActivity.fail).
                refusal = if (GameBuild.offered(context)) {
                    context.getString(R.string.game_code_refused_build_here, problem, context.getString(R.string.setup_command))
                } else {
                    context.getString(R.string.game_code_refused, problem)
                }
            }
        }
        return refusal ?: context.getString(if (GameBuild.offered(context)) R.string.build_code_missing else R.string.game_code_missing)
    }

    /** A statement's shape: lines of a name and a value, the first the title's. */
    private val STATEMENT = Regex("title [a-z0-9]+\n([a-z-]+ [0-9a-f]+\n)+")
}
