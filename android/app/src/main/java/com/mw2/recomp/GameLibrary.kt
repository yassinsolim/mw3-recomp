package com.mw2.recomp

import android.content.Context
import android.content.pm.PackageManager
import java.io.File

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
 * built for another version of the app
 * (runtime/android/game_library.cpp).
 */
object GameLibrary {
    /** Whether the game code is apart from the app's own libraries in this build. */
    val separate: Boolean = BuildConfig.GAME_PACKAGE.isNotEmpty()

    /**
     * Where a build on this device leaves [title]'s library: the app's
     * private storage, which only the app can write, and from which Android
     * lets an app load code it wrote (seen on Android 13 with targetSdk 35:
     * it is running a program from there that is refused, not mapping a
     * library).
     */
    fun built(context: Context, title: Title): File = File(File(context.filesDir, "game-code"), "lib${title.library}game.so")

    /** The title's library in the game package, or null when it is not installed. */
    private fun packaged(context: Context, title: Title): File? {
        val folder = try {
            context.packageManager.getApplicationInfo(BuildConfig.GAME_PACKAGE, 0).nativeLibraryDir
        } catch (_: PackageManager.NameNotFoundException) {
            return null
        }
        return File(folder, "lib${title.library}game.so").takeIf { it.isFile }
    }

    /**
     * The title's libraries on this device, the one built here first: it is
     * what the player did last in this app, and a package from a PC that is
     * newer than it is still reached when the runtime refuses this one.
     */
    private fun files(context: Context, title: Title): List<File> =
        listOfNotNull(built(context, title).takeIf { it.isFile }, packaged(context, title))

    /** Whether there is game code on this device at all: what the launcher says before a run. */
    fun installed(context: Context): Boolean =
        !separate || Title.carried.any { files(context, it).isNotEmpty() }

    /** Whether [title]'s game code is on this device: what a run of it needs. */
    fun ready(context: Context, title: Title): Boolean = !separate || files(context, title).isNotEmpty()

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
     */
    fun load(context: Context, title: Title): String? {
        if (!separate) return null
        var refusal: String? = null
        for (library in files(context, title)) {
            val problem = try {
                NativeBridge.nativeLoadGame(library.absolutePath)
            } catch (error: Throwable) {
                error.message ?: error.toString()
            }
            if (problem.isEmpty()) return null
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
}
