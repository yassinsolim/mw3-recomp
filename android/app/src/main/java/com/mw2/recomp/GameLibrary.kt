package com.mw2.recomp

import android.content.Context
import android.content.pm.PackageManager
import java.io.File

/**
 * The recompiled game code, where the app is published without it
 * (BuildConfig.GAME_PACKAGE; docs/game-library.md). It is made from the
 * player's own disc on their PC (setup\setup.cmd) and installed as a package
 * of its own with no code and no screen, whose library folder Android
 * unpacks; this app asks the system where that is and the runtime loads the
 * title's library from there. An update of the app leaves the package alone,
 * and the runtime refuses one built for another version of the app
 * (runtime/android/game_library.cpp).
 */
object GameLibrary {
    /** Whether the game code is a package of its own in this build. */
    val separate: Boolean = BuildConfig.GAME_PACKAGE.isNotEmpty()

    /** The title's library in the game package, or null when it is not installed. */
    private fun file(context: Context, title: Title): File? {
        val folder = try {
            context.packageManager.getApplicationInfo(BuildConfig.GAME_PACKAGE, 0).nativeLibraryDir
        } catch (_: PackageManager.NameNotFoundException) {
            return null
        }
        return File(folder, "lib${title.library}game.so").takeIf { it.isFile }
    }

    /** Whether there is game code on this device at all: what the launcher says before a run. */
    fun installed(context: Context): Boolean =
        !separate || Title.carried.any { file(context, it) != null }

    /** Whether [title]'s game code is on this device: what a run of it needs. */
    fun ready(context: Context, title: Title): Boolean = !separate || file(context, title) != null

    /** The game package and each title's library, for a log (DeviceInfo). */
    fun describe(context: Context): String {
        if (!separate) return "part of this app"
        val version = try {
            context.packageManager.getPackageInfo(BuildConfig.GAME_PACKAGE, 0).versionName
        } catch (_: PackageManager.NameNotFoundException) {
            return "${BuildConfig.GAME_PACKAGE} is not installed (it is built by setup\\setup.cmd on a PC)"
        }
        return "${BuildConfig.GAME_PACKAGE} $version, " + Title.carried.joinToString {
            "lib${it.library}game.so " + if (file(context, it) != null) "is there" else "is MISSING"
        }
    }

    /**
     * Loads the title's game code into this process, after the runtime
     * (NativeBridge.load). Null when it is ready, or what to tell the player.
     */
    fun load(context: Context, title: Title): String? {
        if (!separate) return null
        val library = file(context, title) ?: return context.getString(R.string.game_code_missing)
        val problem = try {
            NativeBridge.nativeLoadGame(library.absolutePath)
        } catch (error: Throwable) {
            error.message ?: error.toString()
        }
        return if (problem.isEmpty()) null else context.getString(R.string.game_code_refused, problem)
    }
}
