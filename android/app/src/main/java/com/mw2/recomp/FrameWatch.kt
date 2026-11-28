package com.mw2.recomp

/**
 * Whether a run's first frames are a picture.
 *
 * The frame counter rising says the driver presents; it does not say what.
 * A player's Snapdragon 8 Gen 2 ran Advanced Warfare at 58 frames a second
 * of coloured speckle on black, and others a black screen that never ended.
 * So until a driver has shown a picture on this phone the runtime looks at a
 * frame twice a second (runtime/gpu/vulkan/picture_probe.h) and this decides
 * over the looks:
 *
 *   GOOD   [GOOD_LOOKS] looks in a row were a picture. Final: the driver
 *          draws, nothing is looked at again in this run, and no later run
 *          on it is watched (GameActivity).
 *   NOISE  [NOISE_LOOKS] looks were judged, no run of pictures among them,
 *          and four in five were speckle.
 *   FLAT   [FLAT_LOOKS] looks were judged and every one was a single colour
 *          the game had drawn on.
 *
 * Both are slow on purpose, and for either the app only asks the player
 * (GameActivity.showPictureWrong): a wrong switch in the middle of a game
 * would be worse than what it mends.
 *
 * What a look is ([kind]) was measured on 19,006 looks at 102 screen
 * recordings of the five games, the visual checks' 45 labelled ones among
 * them (tools/frame_watch_calibrate.py; docs/android.md has the table), and
 * FrameWatchTest runs the numbers of those looks through this class:
 *
 *  - Samples unlike all four neighbours are no sign of anything alone: play
 *    has up to 300 in a thousand, the player's screenshot 158. Speckle is
 *    [SPECKLE] of them or more, on a ground of one colour over [GROUND] of
 *    the frame or more (the screenshot: 587), of which at most one in
 *    [MOVED] is where it was half a second before -- stars, a menu's small
 *    print and a perforated wall stay. 12 looks of the 19,006 are that, six
 *    in a row at the most, and the worst twenty seconds anywhere hold 15%
 *    of them where the verdict wants 80%. Speckle that stands still is not
 *    told from a picture, and is not caught.
 *  - One colour counts when the frame had a draw in it: black that nothing
 *    was drawn on is not the driver's, and such a look is not judged at
 *    all. (Draws tell little more: Modern Warfare 3 on the Thor draws 51 to
 *    99 times a frame under its logos, its videos and the black between
 *    them, and 3,588 under a level's fade.) What keeps a healthy run clear
 *    is time: the longest stretch of one colour in any recording is 8
 *    seconds, where the verdict wants 60.
 */
class FrameWatch {
    enum class Verdict { WATCHING, GOOD, NOISE, FLAT }

    /** What one look was. IDLE: one colour that nothing was drawn on, which says nothing. */
    enum class Kind { IDLE, FLAT, SPECKLE, PICTURE }

    var verdict = Verdict.WATCHING
        private set

    private var lastSerial = 0
    private var judged = 0
    private var flatLooks = 0
    private var speckleLooks = 0
    private var pictureRun = 0

    /**
     * One look, as NativeBridge.nativePictureProbe hands it over: [serial]
     * counts the frames looked at (the same serial twice is the same look,
     * and 0 is none yet). Returns the verdict so far.
     */
    fun look(serial: Int, flat: Boolean, isolated: Int, ground: Int, stable: Int, draws: Int): Verdict {
        if (verdict == Verdict.GOOD || serial <= 0 || serial == lastSerial) return verdict
        lastSerial = serial
        when (kind(flat, isolated, ground, stable, draws)) {
            Kind.IDLE -> return verdict
            Kind.FLAT -> { flatLooks++; pictureRun = 0 }
            Kind.SPECKLE -> { speckleLooks++; pictureRun = 0 }
            Kind.PICTURE -> pictureRun++
        }
        judged++
        verdict = when {
            pictureRun >= GOOD_LOOKS -> Verdict.GOOD
            judged >= NOISE_LOOKS && speckleLooks * 5 >= judged * 4 -> Verdict.NOISE
            judged >= FLAT_LOOKS && flatLooks == judged -> Verdict.FLAT
            else -> Verdict.WATCHING
        }
        return verdict
    }

    /** How the looks so far went, for the log. */
    fun counts(): String = "$judged looks judged: $flatLooks one colour, $speckleLooks speckle"

    companion object {
        /** Three seconds of pictures. */
        const val GOOD_LOOKS = 6
        /** Twenty seconds of looks. */
        const val NOISE_LOOKS = 40
        /** A minute of looks. */
        const val FLAT_LOOKS = 120
        /** Per thousand samples unlike all four neighbours. */
        const val SPECKLE = 80
        /** Per thousand samples of the frame's middle colour. */
        const val GROUND = 333
        /** Speckle moved when at most one sample in this many is isolated where it was, and the colour it was. */
        const val MOVED = 4
        /** Draws in a frame of one colour that make it the game's: any. */
        const val DRAWS = 1

        /** Per thousand: [isolated], [ground], [stable] (picture_probe.h); [draws] the frame's. */
        fun kind(flat: Boolean, isolated: Int, ground: Int, stable: Int, draws: Int): Kind = when {
            flat -> if (draws >= DRAWS) Kind.FLAT else Kind.IDLE
            isolated >= SPECKLE && ground >= GROUND && stable * MOVED <= isolated -> Kind.SPECKLE
            else -> Kind.PICTURE
        }
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
