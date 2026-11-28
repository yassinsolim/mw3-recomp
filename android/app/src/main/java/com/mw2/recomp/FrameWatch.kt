package com.mw2.recomp

/**
 * Whether a run's first frames are a picture.
 *
 * The frame counter rising says the driver presents; it does not say what.
 * A player's Snapdragon 8 Gen 2 ran Advanced Warfare at 58 frames a second
 * of coloured blocks, and others a black screen that never ended. So for a
 * run's first minutes the runtime looks at a frame twice a second
 * (runtime/gpu/vulkan/picture_probe.h: one colour? how many of its samples
 * are unlike all their neighbours?) and this decides over time:
 *
 *   GOOD   [goodLooks] looks in a row were a picture. Final: the driver
 *          draws, and nothing is looked at again.
 *   NOISE  [noiseMs] of looks went by, no run of good ones among them, and
 *          four in five were noise.
 *   FLAT   [flatMs] of looks went by and every one was a single colour.
 *
 * A healthy start is black for a while, then logos and a menu, which are
 * pictures: neither verdict is reached. Both are slow on purpose and the
 * app only offers another driver for them (GameActivity), never switches by
 * itself: a long dark loading screen is not a broken driver.
 */
class FrameWatch(
    private val noiseAt: Int = NOISE_AT,
    private val goodLooks: Int = 6,
    private val noiseMs: Long = 20_000,
    private val flatMs: Long = 60_000,
) {
    enum class Verdict { WATCHING, GOOD, NOISE, FLAT }

    var verdict = Verdict.WATCHING
        private set

    private var lastSerial = 0
    private var firstAt = -1L
    private var looks = 0
    private var flatLooks = 0
    private var noiseLooks = 0
    private var goodRun = 0

    /**
     * One look: [serial] counts the frames looked at (the same serial twice
     * is the same look, and 0 is none yet), [flat] and [isolated] are the
     * runtime's (per thousand samples unlike all four neighbours).
     * [nowMs] is any steady clock. Returns the verdict so far.
     */
    fun look(nowMs: Long, serial: Int, flat: Boolean, isolated: Int): Verdict {
        if (verdict == Verdict.GOOD || serial <= 0 || serial == lastSerial) return verdict
        lastSerial = serial
        if (firstAt < 0) firstAt = nowMs
        looks++
        val noise = !flat && isolated >= noiseAt
        when {
            flat -> { flatLooks++; goodRun = 0 }
            noise -> { noiseLooks++; goodRun = 0 }
            else -> goodRun++
        }
        val watched = nowMs - firstAt
        verdict = when {
            goodRun >= goodLooks -> Verdict.GOOD
            watched >= noiseMs && noiseLooks * 5 >= looks * 4 -> Verdict.NOISE
            watched >= flatMs && flatLooks == looks -> Verdict.FLAT
            else -> Verdict.WATCHING
        }
        return verdict
    }

    /** The player said to go on: the watch starts over from the next look. */
    fun startOver() {
        if (verdict == Verdict.GOOD) return
        verdict = Verdict.WATCHING
        firstAt = -1
        looks = 0
        flatLooks = 0
        noiseLooks = 0
        goodRun = 0
    }

    companion object {
        /**
         * Per thousand. The player's screenshot of noise measures 158; the
         * busiest frames of the two test phones' own stills 95 and 111
         * (docs/android.md), which is why noise is a verdict over twenty
         * seconds with no picture among them and never one frame's.
         */
        const val NOISE_AT = 120
    }
}

/** What to do when a run did not show the game, or died in the driver. */
object DriverFallback {
    /**
     * Which driver comes next: the best of [ranked] (DriverCatalog.ranked
     * for this phone, what has failed left out) that is not the one that
     * just failed and that [ready] says is on the phone; null where nothing
     * is left to try.
     */
    fun next(ranked: List<DriverCatalog.Choice>, failedId: String, ready: (DriverCatalog.Choice) -> Boolean): DriverCatalog.Choice? =
        ranked.firstOrNull { it.id != failedId && ready(it) }

    /**
     * How many runs on a driver may end in a crash inside it before the
     * app stops choosing it: one crash can be anything.
     */
    const val STRIKES = 2

    private val driverFiles = listOf(
        "/hw/vulkan.", "vulkan.adreno", "vulkan.mali", "libllvm-qgl", "libllvm-glnext", "libgsl.so",
        "libvulkan_freedreno", "libvulkan_panfrost", "libGLES_mali", "/files/drivers/", "/driver_tmp/",
    )

    /**
     * Whether a crash was inside the graphics driver: one of the crashing
     * thread's innermost six frames is in a driver's file. [trace] is
     * Tombstone.read's text; [library] the selected driver's file name, ""
     * for the phone's own.
     */
    fun crashInDriver(trace: String, library: String): Boolean =
        trace.lineSequence().map { it.trim() }.filter { it.startsWith("#") }.take(6).any { frame ->
            driverFiles.any { frame.contains(it) } || (library.isNotEmpty() && frame.contains(library))
        }
}
