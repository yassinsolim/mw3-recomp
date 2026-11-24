package com.mw2.recomp

import android.content.Context
import java.io.File

/**
 * The disc's two executables: the campaign's (with Special Ops) and the
 * multiplayer's. Each is recompiled into a native library of its own, and an
 * app carries one of them or, for Modern Warfare 3, both (build.gradle.kts,
 * LIBRARY_SP and LIBRARY_MP). Both export the same JNI names, so a process
 * loads exactly one: the launcher's the first the app carries, the run's the
 * one the player chose.
 */
enum class Title(val library: String, val executable: String) {
    CAMPAIGN(BuildConfig.LIBRARY_SP, "default.xex"),
    MULTIPLAYER(BuildConfig.LIBRARY_MP, "default_mp.xex");

    /** Whether this app carries the title at all. */
    val carried: Boolean
        get() = library.isNotEmpty()

    /** Whether its executable is in the game folder. */
    fun installed(gameDirectory: String): Boolean =
        carried && gameDirectory.isNotEmpty() && File(gameDirectory, executable).isFile

    /**
     * What sets this title's shader and pipeline caches apart from the other's
     * in an app that carries both: each run warms what its cache recorded, and
     * the campaign's shaders are no use to a multiplayer match. An app of one
     * title keeps the names it always had.
     */
    val cacheSuffix: String
        get() = if (this == MULTIPLAYER && both) "_mp" else ""

    /**
     * The folder the title's saves/ is in: the one the game folder is in, as
     * it always was. The multiplayer of the app that carries both has mp/
     * there instead. The two were apps of their own before, each with its
     * profile and its rank, and stay apart; and what the old multiplayer app
     * kept is this folder's contents unchanged (OldMultiplayerApp).
     */
    fun home(gameDirectory: File): File {
        val beside = gameDirectory.absoluteFile.parentFile ?: gameDirectory
        return if (this == MULTIPLAYER && both) File(beside, "mp") else beside
    }

    companion object {
        /** The titles this app carries, the campaign first. */
        val carried: List<Title> = entries.filter { it.carried }

        /** The one app with the whole game. */
        val both: Boolean = carried.size > 1

        /** com.mw3.recomp* is Modern Warfare 3; everything else here is MW2, */
        val isMw3: Boolean = BuildConfig.APPLICATION_ID.startsWith("com.mw3.")

        /**
         * but for com.cod4.recomp*, Call of Duty 4, which goes the way of
         * Modern Warfare 2's one app wherever this does not say otherwise:
         * the disc's version alone, and a system link for its multiplayer.
         */
        val isCod4: Boolean = BuildConfig.APPLICATION_ID.startsWith("com.cod4.")

        /** com.aw.recomp*, Call of Duty: Advanced Warfare, which goes the same way. */
        val isAw: Boolean = BuildConfig.APPLICATION_ID.startsWith("com.aw.")

        /**
         * Advanced Warfare is two discs: whether the install disc's two
         * content packages, which the game mounts as a console does from its
         * hard disk, are still to be copied beside the game folder
         * (runtime/install/install.cpp, runtime/kernel/content.cpp).
         */
        fun installDiscMissing(gameDirectory: String): Boolean {
            if (!isAw || gameDirectory.isEmpty()) return false
            val packages = File(File(gameDirectory).absoluteFile.parentFile, "dlc")
            return !listOf("41560914051BF3C0", "4156091407406280").all { File(packages, it).isFile }
        }

        /**
         * Whether this app's multiplayer has bots (runtime/bots.cpp): the
         * disc executables of both Modern Warfare games and of Call of Duty 4
         * have, Modern Warfare 2's title update 6 has not.
         */
        val hasBots: Boolean = MULTIPLAYER.carried && BuildConfig.MW2_TU == "tu0"

        /** Modern Warfare 2, either version of it. */
        val isMw2: Boolean = !isMw3 && !isCod4 && !isAw

        /**
         * Detail at distance (MW2_LOD_BIAS): the hook is in Modern Warfare
         * 2's disc executables alone (runtime/model_detail.cpp).
         */
        val hasDetailBias: Boolean = isMw2 && BuildConfig.MW2_TU == "tu0"

        /** The title an executable's name on the disc belongs to, if this app carries it. */
        fun forExecutable(name: String): Title? = carried.firstOrNull { it.executable == name }
    }
}

/**
 * Modern Warfare 3's multiplayer as the app of its own it used to be
 * (com.mw3.recomp.mp): its profile and rank are in that app's saves folder,
 * which is exactly what the multiplayer of this app keeps in mp/saves
 * (Title.home).
 *
 * Best effort, and said so: from Android 11 on an app cannot read another
 * app's folder under Android/data whatever permission it holds, so on most
 * phones this finds nothing and setup\setup.ps1 moves the folder over adb
 * instead. Where the system does allow it, the first start does it here.
 */
object OldMultiplayerApp {
    private const val SAVES = "/sdcard/Android/data/com.mw3.recomp.mp/files/saves"

    /** Copies the old app's saves in, once: never over saves already here. */
    fun adoptSaves(gameDirectory: File) {
        if (!Title.both || !Title.isMw3) return
        val home = Title.MULTIPLAYER.home(gameDirectory)
        val saves = File(home, "saves")
        val old = File(SAVES)
        try {
            if (saves.exists() || !old.isDirectory || old.list() == null) return
            // Under another name until it is whole: a copy cut short must not
            // be taken for the player's saves the next time.
            val partial = File(home, "saves.part")
            partial.deleteRecursively()
            if (old.copyRecursively(partial)) partial.renameTo(saves) else partial.deleteRecursively()
        } catch (_: Throwable) {
            // Not readable after all: the multiplayer starts a fresh profile.
        }
    }
}

/**
 * What the next run is to start: which title, and the launch data the other
 * title left for it (the console's XamLoaderSetLaunchData, as hexadecimal),
 * empty for a start from the launcher.
 *
 * It is a file rather than a preference or an intent extra because it crosses
 * processes at the moment one of them is dying. SharedPreferences are read
 * once per process, so a run's process still alive from the last run would
 * see the title it was started with; and Android restores a GameActivity
 * whose process died with the intent it first had, not the newest one.
 */
class RunRequest(val title: Title, val launchData: String) {

    /** Written whole or not at all: through a temporary name. */
    fun write(context: Context) {
        val partial = File(context.filesDir, "$FILE.part")
        partial.writeText("${title.name}\n$launchData\n")
        partial.renameTo(file(context))
    }

    companion object {
        private const val FILE = "run_request.txt"

        private fun file(context: Context) = File(context.filesDir, FILE)

        /** The last request, or the app's first title when there has been none. */
        fun read(context: Context): RunRequest {
            val lines = try {
                file(context).readLines()
            } catch (_: Throwable) {
                emptyList()
            }
            val title = Title.carried.firstOrNull { it.name == lines.getOrNull(0) } ?: Title.carried.first()
            return RunRequest(title, lines.getOrNull(1).orEmpty())
        }
    }
}
