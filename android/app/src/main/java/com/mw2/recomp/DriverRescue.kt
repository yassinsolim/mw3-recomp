package com.mw2.recomp

import android.content.Context
import android.os.Build
import java.io.File

/**
 * A player's own choice of driver that does not run the game: offered the
 * driver recommended for the phone, once, and never switched without them.
 *
 * Asked at the launcher's next start when the driver in use is the player's
 * choice (Prefs.driverChoiceMade) and either the catalog calls it broken for
 * this chip and game (a report, DriverCatalog.brokenHere), or the last
 * [DriverFallback.STRIKES] runs on it in a row ended in a crash inside the
 * driver's files or before the game's first picture ([classify], over
 * Android's record of each run's end and the run's own start-up steps in
 * game.log). "Use recommended" hands the choice back to the app, which
 * selects and tests the recommended driver (DriverAuto.run); the player's
 * driver stays installed. "Keep mine" is remembered for that driver's
 * version ([keepKey]): a new version is asked about again.
 */
object DriverRescue {

    /** How a run ended, as far as its driver goes. */
    enum class End { DRIVER_CRASH, NO_PICTURE, OTHER }

    /** A run's end and, for a crash inside the driver, the driver's file it was in. */
    data class Verdict(val end: End, val library: String = "")

    /** The start-up steps (runtime crash::Step) that are the graphics driver's work. */
    val GRAPHICS_STEPS = setOf(
        "gpu", "vulkan-library", "surface", "vulkan-instance", "vulkan-surface", "vulkan-device",
        "swapchain", "pipeline-layout", "textures",
    )

    private const val RUN_MARKER = "===== new run, pid "
    private val faultMarkers = listOf("---- guest fault", "---- the process is ending")
    private val stepLine = Regex("""\bstep: ([a-z0-9-]+) \(""")
    private val lastStepLine = Regex("""last step: ([a-z0-9-]+)""")

    /** What a run's part of game.log says: its last start-up step, whether a fault was reported, and the fault's backtrace. */
    data class RunLog(val lastStep: String, val faulted: Boolean, val backtrace: String)

    /** The part of [gameLog] (every run's, one after another) that is the run of process [pid], or null where it is not there. */
    fun runLog(gameLog: String, pid: Int): RunLog? {
        val start = gameLog.lastIndexOf("$RUN_MARKER$pid =====")
        if (start < 0) return null
        val next = gameLog.indexOf(RUN_MARKER, start + RUN_MARKER.length)
        val lines = gameLog.substring(start, if (next < 0) gameLog.length else next).lines()
        val faultAt = lines.indexOfFirst { line -> faultMarkers.any { it in line } }
        val before = if (faultAt >= 0) lines.subList(0, faultAt) else lines
        val fault = if (faultAt >= 0) lines.subList(faultAt, lines.size) else emptyList()
        val step = fault.firstNotNullOfOrNull { lastStepLine.find(it)?.groupValues?.get(1) }
            ?: before.lastOrNull { stepLine.containsMatchIn(it) }?.let { stepLine.find(it)!!.groupValues[1] }
            ?: ""
        val traceFrom = fault.indexOfFirst { "fault backtrace" in it }
        val trace = if (traceFrom < 0) "" else fault.drop(traceFrom + 1).takeWhile { !it.contains("--------") }.joinToString("\n")
        return RunLog(step, faultAt >= 0, trace)
    }

    /**
     * How the run ended. [nativeCrash]: Android recorded a native crash;
     * [tombstone]: Tombstone.read's text of it, where the system kept one
     * (Android 12 on); [run]: the run's game.log ([runLog]); [framed]: the
     * run showed its first frame (null: not known); [library]: the selected
     * driver's file ("" for the phone's own).
     *
     * A crash inside the driver is told by the crashing thread's innermost
     * frames, the tombstone's or else the runtime's own fault report's. A
     * crash anywhere before the first frame counts as one before the picture
     * when start-up had got to the driver's steps; a crash in the game's own
     * code after start-up (the "guest" step) is neither.
     */
    fun classify(nativeCrash: Boolean, tombstone: String?, run: RunLog?, framed: Boolean?, library: String): Verdict {
        if (!nativeCrash && run?.faulted != true) return Verdict(End.OTHER)
        val file = tombstone?.let { DriverFallback.driverLibraryOf(it, library) }
            ?: run?.let { DriverFallback.driverLibraryOf(it.backtrace, library) }
        if (file != null) return Verdict(End.DRIVER_CRASH, file)
        if (framed != true && run != null && run.lastStep in GRAPHICS_STEPS) return Verdict(End.NO_PICTURE)
        return Verdict(End.OTHER)
    }

    /**
     * The runs in a row on driver [id] that ended badly: [crashes] inside
     * it ([library] the last file named), [noPicture] before the picture.
     */
    data class Streak(val id: String, val crashes: Int, val noPicture: Int, val library: String) {
        val count: Int get() = crashes + noPicture

        /** After one more run on [runId]: a run that ended well, or on another driver, starts again. */
        fun after(runId: String, verdict: Verdict): Streak = when {
            verdict.end == End.OTHER -> NONE
            runId != id -> Streak(runId, 0, 0, "").after(runId, verdict)
            verdict.end == End.DRIVER_CRASH -> copy(crashes = crashes + 1, library = verdict.library)
            else -> copy(noPicture = noPicture + 1)
        }

        /** What happened, in English as the log is. */
        fun why(): String =
            if (crashes >= noPicture) "the game crashed inside it ($library) on the last $count starts"
            else "the game stopped before its first picture on the last $count starts"

        override fun toString(): String = "$id\t$crashes\t$noPicture\t$library"

        companion object {
            val NONE = Streak("", 0, 0, "")

            fun parse(text: String): Streak {
                val parts = text.split('\t')
                if (parts.size != 4 || parts[0].isEmpty()) return NONE
                return Streak(parts[0], parts[1].toIntOrNull() ?: 0, parts[2].toIntOrNull() ?: 0, parts[3])
            }
        }
    }

    /**
     * What "Keep mine" is remembered for: the driver by the name its results
     * are kept under, and its version -- the package's, or for the phone's
     * own the OpenGL ES driver's line, which names the version.
     */
    fun keepKey(id: String, driver: DriverStore.Driver?, gpuText: String): String =
        if (driver != null) "$id|${driver.version}|${driver.description}" else "$id|$gpuText"

    /** The question to put, with what it is about. */
    class Offer(
        val id: String,
        val name: String,
        /** What happened, for the player. */
        val why: String,
        /** The same, in English, for the log and the driver's verdict. */
        val whyLog: String,
        /** The driver recommended for this phone, or null where the catalog has nothing else. */
        val recommended: String?,
        val keepKey: String,
    )

    /** The question for this launch, or null. Reads the catalog; not for the main thread. */
    fun offer(context: Context, prefs: Prefs): Offer? {
        if (!prefs.driverChoiceMade) return null
        val catalog = DriverAuto.catalog(context)
        val text = DriverAuto.gpuText(context)
        val gpu = Gpu.parse(text)
        val (driver, id) = DriverAuto.current(context, prefs, catalog)
        val key = keepKey(id, driver, text)
        if (prefs.driverRescueKept == key) return null
        val streak = Streak.parse(prefs.driverStreak)
        val (why, whyLog) = when {
            streak.id == id && streak.count >= DriverFallback.STRIKES ->
                if (streak.crashes >= streak.noPicture) {
                    context.getString(R.string.driver_rescue_crashed, streak.library, streak.count) to streak.why()
                } else {
                    context.getString(R.string.driver_rescue_no_picture, streak.count) to streak.why()
                }
            catalog.brokenHere(id, gpu, DriverCatalog.game, text) != null ->
                context.getString(R.string.driver_rescue_broken) to "the catalog reports it broken for ${gpu.name} in ${DriverCatalog.game}"
            else -> return null
        }
        val best = catalog.ranked(gpu, Build.VERSION.SDK_INT, DriverCatalog.game, text, DriverAuto.failed(prefs) + id)
            .firstOrNull { it.id != id }
        val name = driver?.name ?: context.getString(R.string.driver_system)
        return Offer(id, name, why, whyLog, best?.let { it.entry?.name ?: context.getString(R.string.driver_system) }, key)
    }

    /**
     * "Use recommended": the player's driver is written down as failed here
     * (it stays installed and listed), and the choice goes back to the app,
     * whose next pass (DriverAuto.run, forced) selects and tests the
     * recommended driver.
     */
    fun useRecommended(context: Context, prefs: Prefs, offer: Offer) {
        prefs.noteDriverVerdict(offer.id, "fail: ${offer.whyLog}")
        prefs.driverChoiceMade = false
        prefs.driverStreak = ""
        prefs.driverAutoKey = ""
        prefs.flush()
        DriverAuto.log(context, "the player takes the recommended ${offer.recommended} over ${offer.id}, which ${offer.whyLog}")
    }

    /** "Keep mine": not asked again while this version of the driver is in use. */
    fun keep(context: Context, prefs: Prefs, offer: Offer) {
        prefs.driverRescueKept = offer.keepKey
        prefs.driverStreak = ""
        prefs.flush()
        DriverAuto.log(context, "the player keeps ${offer.id} (${offer.whyLog}); not asked again for this version")
    }

    /** game.log's last megabyte: the runs whose ends are being read are in it. */
    fun gameLog(context: Context): String = File(context.filesDir, "game.log").readTextTail(1 shl 20)
}
