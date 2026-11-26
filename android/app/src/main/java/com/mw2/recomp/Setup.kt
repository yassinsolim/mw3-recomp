package com.mw2.recomp

import android.content.Context
import java.io.File

/**
 * Where a player is on the way from a new app to a game, told from what is
 * on the device and nothing else: the guided setup (SetupActivity) shows the
 * step this names, and the launcher its one large button (MainActivity).
 * Nothing of it is kept apart from the facts, so a setup that was left, or
 * whose process the system ended, or that failed, is at the same step the
 * next time it is looked at.
 *
 * A player asked which of "Build" and "Install files" came first, and
 * whether the multiplayer was installed by itself: the two were entries of
 * equal weight, in the reverse of the order they are done in, beside a Play
 * button that could start nothing yet. Here they are one way with one
 * button.
 */
object Setup {
    enum class Step {
        /** The player's disc image is chosen, and checked to be this game's. */
        DISC,

        /** Advanced Warfare's other disc: the one of its two that is still to be chosen. */
        SECOND_DISC,

        /** The game is built from the disc, campaign and multiplayer together. */
        BUILD,

        /** The player's own map packs, once, for whoever has them. */
        DLC,

        DONE,
    }

    /**
     * The apps with the whole game in them, which are the four that are
     * published: the older apps of one title each (and of title update 6,
     * which is downloaded after the install) keep their install screen. So
     * does Black Ops III, whose game code is built from its title update:
     * its steps are the disc, Add DLC and then the build, not this order.
     */
    val guided: Boolean = Title.both && !TitleUpdate.required

    /**
     * The game folder: the app's own external folder, which needs no
     * permission to write, survives an update and goes with the app.
     */
    fun gameFolder(context: Context): File = File(context.getExternalFilesDir(null) ?: context.filesDir, "game")

    /**
     * A disc image the game is played from that cannot be read any more (a
     * memory card taken out, a file moved or deleted), or null: the game
     * disc's first.
     */
    fun lostDisc(context: Context): String? {
        val prefs = Prefs(context)
        return listOf(prefs.discImage, prefs.installDiscImage).firstOrNull {
            it.isNotEmpty() && DiscImage.source(context, it).also { source -> DiscImage.close(source) } == null
        }
    }

    /** Whether every title's executable is in the game folder: the game disc is set up. */
    private fun gameDisc(context: Context): Boolean = Prefs(context).gameDirectory.let { folder -> Title.carried.all { it.installed(folder) } }

    /**
     * Whether the game has its files: both titles', Advanced Warfare's
     * install disc's, and the images they are read from, [lost] being the
     * one of those that is gone ([lostDisc]).
     */
    private fun filesReady(context: Context, lost: String?): Boolean {
        val prefs = Prefs(context)
        return gameDisc(context) && !Title.installDiscMissing(prefs.gameDirectory, prefs.installDiscImage) && lost == null
    }

    fun filesReady(context: Context): Boolean = filesReady(context, lostDisc(context))

    /**
     * Whether the game code this version of the app runs is here for every
     * title of the disc. A player's title update is not counted: its code is
     * Add DLC's to build (DlcActivity).
     */
    fun codeReady(context: Context): Boolean = !GameLibrary.separate || Title.carried.all { GameLibrary.current(context, it.library) }

    /** Whether the game can be played: nothing is left for the guided setup to do. */
    fun complete(context: Context): Boolean = filesReady(context) && codeReady(context)

    /** The step the player is at. [DLC] is put once, in a first setup (Prefs.setupFinished). */
    fun step(context: Context): Step {
        val prefs = Prefs(context)
        val lost = lostDisc(context)
        return when {
            // A game disc's image that has gone is chosen again as a first one is.
            !gameDisc(context) || (lost != null && lost == prefs.discImage) -> Step.DISC
            !filesReady(context, lost) -> Step.SECOND_DISC
            !codeReady(context) -> Step.BUILD
            !prefs.setupFinished -> Step.DLC
            else -> Step.DONE
        }
    }

    /** How many steps the player is shown: the disc, the build and the map packs. */
    const val STEPS = 3

    /** The step's number as the player counts them, Advanced Warfare's second disc being part of the first. */
    fun number(step: Step): Int = when (step) {
        Step.DISC, Step.SECOND_DISC -> 1
        Step.BUILD -> 2
        Step.DLC, Step.DONE -> 3
    }

    /**
     * The name of the game a disc is, by its title's number, for the four
     * this project has an app of; null for any other. What the setup says
     * of a disc that is not this app's: "This is Modern Warfare 3. This app
     * is for Modern Warfare 2."
     */
    fun gameName(context: Context, title: Long? = Title.titleId): String? = when (title) {
        0x41560817L -> R.string.game_mw2
        0x415608CBL -> R.string.game_mw3
        0x415607E6L -> R.string.game_cod4
        0x41560914L -> R.string.game_aw
        else -> null
    }?.let { context.getString(it) }

    /** The README's quick start in this game's repository, for what the app cannot do itself. */
    fun guide(): String = "https://github.com/${BuildConfig.UPDATE_REPO.ifEmpty { "yassinsolim/mw3-recomp" }}#quick-start"

    /** How far a build is, of a hundred: one bar for the download, the preparing, the compiling and the linking. */
    fun percent(progress: GameBuild.Progress): Int {
        fun part(from: Int, to: Int): Int =
            if (progress.total <= 0) from else from + (to - from) * progress.done.coerceIn(0, progress.total) / progress.total
        return when (progress.step) {
            GameBuild.Step.IDLE -> 0
            // The download is over in a minute; unpacking (a count of files) follows it.
            GameBuild.Step.TOOLCHAIN -> part(0, 8)
            GameBuild.Step.PREPARE -> if (progress.total <= 0) 0 else part(8, 12)
            GameBuild.Step.RECOMPILE -> part(12, 22)
            // Nearly all of the time (docs/ondevice-build.md).
            GameBuild.Step.COMPILE -> part(22, 97)
            GameBuild.Step.LINK -> 98
        }
    }

    /** What a build is doing, in a line a player reads: for the setup's screen, the launcher and the notification. */
    fun progressLine(context: Context, progress: GameBuild.Progress): String = when (progress.step) {
        GameBuild.Step.IDLE -> context.getString(R.string.setup_build_starting)
        GameBuild.Step.TOOLCHAIN -> context.getString(R.string.setup_build_tools, progress.done, progress.total)
        GameBuild.Step.PREPARE, GameBuild.Step.RECOMPILE -> context.getString(R.string.setup_build_preparing)
        GameBuild.Step.LINK -> context.getString(R.string.setup_build_finishing)
        GameBuild.Step.COMPILE -> context.getString(R.string.setup_build_percent, percent(progress)) + " " + when {
            progress.secondsLeft < 0 -> context.getString(R.string.setup_build_left_unknown)
            progress.secondsLeft < 90 -> context.getString(R.string.setup_build_left_soon)
            else -> context.getString(R.string.setup_build_left_minutes, (progress.secondsLeft + 30) / 60)
        }
    }
}
