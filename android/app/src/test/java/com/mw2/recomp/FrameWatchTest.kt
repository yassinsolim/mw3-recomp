package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The watch over a run's first frames (FrameWatch.kt), on looks as the
 * runtime hands them over twice a second, made up and measured (the numbers
 * of every look at 102 recordings of the five games and at the test phones'
 * own starts: no picture of any); which driver comes after one that failed;
 * what the self-test's report file says, whole or cut short; and whether a
 * crash was inside the driver.
 */
class FrameWatchTest {

    /** One look: one colour, or per thousand isolated, ground, stable; and the frame's draws. */
    private class Look(val flat: Boolean, val isolated: Int, val ground: Int, val stable: Int, val draws: Int) {
        val kind get() = FrameWatch.kind(flat, isolated, ground, stable, draws)
    }

    /** [looks] from serial [from]; returns the verdict after the last. */
    private fun feed(watch: FrameWatch, from: Int, looks: List<Look>): FrameWatch.Verdict {
        var verdict = watch.verdict
        looks.forEachIndexed { index, look ->
            verdict = watch.look(from + index, look.flat, look.isolated, look.ground, look.stable, look.draws)
        }
        return verdict
    }

    private val loading = Look(true, 0, 1000, 0, 0)       // black, nothing drawn on it
    private val black = Look(true, 0, 1000, 0, 60)        // black, with a menu's draws under it
    private val picture = Look(false, 30, 200, 28, 60)
    private val play = Look(false, 280, 60, 20, 2800)     // foliage in a turn: unlike its neighbours, no ground
    private val stars = Look(false, 160, 700, 150, 40)    // specks on black that stay where they are
    private val speckle = Look(false, 158, 587, 12, 60)   // the player's screenshot, elsewhere every look

    @Test
    fun aLookIsOneOfFourKinds() {
        assertEquals(FrameWatch.Kind.IDLE, loading.kind)
        assertEquals(FrameWatch.Kind.FLAT, black.kind)
        assertEquals(FrameWatch.Kind.PICTURE, picture.kind)
        assertEquals(FrameWatch.Kind.PICTURE, play.kind)
        assertEquals(FrameWatch.Kind.PICTURE, stars.kind)
        assertEquals(FrameWatch.Kind.SPECKLE, speckle.kind)
        // Each line itself.
        assertEquals(FrameWatch.Kind.FLAT, FrameWatch.kind(true, 0, 1000, 0, FrameWatch.DRAWS))
        assertEquals(FrameWatch.Kind.IDLE, FrameWatch.kind(true, 0, 1000, 0, FrameWatch.DRAWS - 1))
        assertEquals(FrameWatch.Kind.SPECKLE, FrameWatch.kind(false, FrameWatch.SPECKLE, FrameWatch.GROUND, FrameWatch.SPECKLE / FrameWatch.MOVED, 0))
        assertEquals(FrameWatch.Kind.PICTURE, FrameWatch.kind(false, FrameWatch.SPECKLE - 1, FrameWatch.GROUND, 0, 0))
        assertEquals(FrameWatch.Kind.PICTURE, FrameWatch.kind(false, FrameWatch.SPECKLE, FrameWatch.GROUND - 1, 0, 0))
        assertEquals(FrameWatch.Kind.PICTURE, FrameWatch.kind(false, FrameWatch.SPECKLE, FrameWatch.GROUND, FrameWatch.SPECKLE / FrameWatch.MOVED + 1, 0))
        // The first look has none before it and the runtime calls all its specks stable: never speckle.
        assertEquals(FrameWatch.Kind.PICTURE, FrameWatch.kind(false, 158, 587, 158, 60))
    }

    @Test
    fun aHealthyStartIsBlackThenAPicture() {
        val watch = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(40) { loading }))
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 41, List(5) { picture }))
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 46, listOf(picture)))
        // Good is final: whatever comes afterwards is the game's own business.
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 47, List(200) { speckle }))
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 247, List(200) { black }))
    }

    @Test
    fun speckleForTwentySecondsIsNoise() {
        val watch = FrameWatch()
        // Nineteen and a half seconds of it: not yet.
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(39) { speckle }))
        assertEquals(FrameWatch.Verdict.NOISE, feed(watch, 40, listOf(speckle)))
        // A loading screen between is not judged, and does not put the verdict off for good.
        val broken = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(broken, 1, List(30) { speckle } + List(100) { loading }))
        assertEquals(FrameWatch.Verdict.NOISE, feed(broken, 131, List(10) { speckle }))
    }

    @Test
    fun theOddFrameThatMeasuresAsAPictureDoesNotHideSpeckle() {
        val watch = FrameWatch()
        // One look in six is a picture by the numbers, never six in a row.
        assertEquals(FrameWatch.Verdict.NOISE, feed(watch, 1, List(48) { if (it % 6 == 5) picture else speckle }))
    }

    @Test
    fun busyPlayAndSpecksThatStayAreNotNoise() {
        // Play from the first look, as busy as play gets, for three minutes.
        assertEquals(FrameWatch.Verdict.GOOD, feed(FrameWatch(), 1, List(6) { play }))
        // A starfield or a page of small print on black.
        assertEquals(FrameWatch.Verdict.GOOD, feed(FrameWatch(), 1, List(6) { stars }))
        // Speckle one look in three among pictures: never four in five.
        val mixed = FrameWatch()
        assertFalse(feed(mixed, 1, List(360) { if (it % 3 == 0) speckle else if (it % 3 == 1) picture else black }) == FrameWatch.Verdict.NOISE)
    }

    @Test
    fun oneColourForAMinuteIsFlatAndNotBefore() {
        val watch = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(119) { black }))
        assertEquals(FrameWatch.Verdict.FLAT, feed(watch, 120, listOf(black)))
        // Three minutes of a loading screen, nothing drawn: not judged at all.
        val slow = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(slow, 1, List(360) { loading }))
        assertEquals(FrameWatch.Verdict.GOOD, feed(slow, 361, List(6) { picture }))
        // One picture among the black: the driver can draw, and one colour is the game's.
        val once = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(once, 1, List(60) { black } + picture + List(300) { black }))
    }

    @Test
    fun aLookIsCountedOnceAndNoneIsNothing() {
        val watch = FrameWatch()
        // No frame looked at yet (serial 0), for minutes: the black-screen watch's business, not this one's.
        repeat(300) { assertEquals(FrameWatch.Verdict.WATCHING, watch.look(0, true, 0, 1000, 0, 60)) }
        // The same look read three hundred times is one look.
        repeat(300) { assertEquals(FrameWatch.Verdict.WATCHING, watch.look(1, true, 0, 1000, 0, 60)) }
        assertEquals("1 looks judged: 1 one colour, 0 speckle", watch.counts())
    }

    /** A fixture's recordings: a line each, `name<TAB>looks`; a look `isolated.ground.stable[.draws]` or `F[.draws]`. */
    private fun recordings(resource: String): Map<String, List<Look>> {
        val stream = javaClass.classLoader!!.getResourceAsStream(resource) ?: error("$resource is not among the test's resources")
        return stream.bufferedReader().readLines().filter { it.isNotBlank() }.associate { line ->
            val (name, looks) = line.split('\t')
            name to looks.split(' ').map { look ->
                val fields = look.split('.')
                // A screen recording does not say what was drawn: every one-colour look counts, the hard way round.
                if (fields[0] == "F") Look(true, 0, 1000, 0, fields.getOrNull(1)?.toInt() ?: FrameWatch.DRAWS)
                else Look(false, fields[0].toInt(), fields[1].toInt(), fields[2].toInt(), fields.getOrNull(3)?.toInt() ?: FrameWatch.DRAWS)
            }
        }
    }

    private fun longestRun(looks: List<Look>, of: FrameWatch.Kind): Int {
        var best = 0
        var run = 0
        for (look in looks) {
            run = if (look.kind == of) run + 1 else 0
            best = maxOf(best, run)
        }
        return best
    }

    /**
     * The recordings of real runs (tools/frame_watch_calibrate.py wrote
     * their numbers): menus, videos, loading screens, play in the five
     * games on both phones, the visual checks' labelled faults among them;
     * and the looks the runtime itself made at starts on the two phones
     * (frame_looks_phones.tsv, from logs written with MW2_PICTURE_PROBE_LOG:
     * what the presenter copies, the app's overlay not in it, with each
     * frame's draws). None is a driver that draws nothing, so wherever in
     * one the watch begins it must never accuse, and the margins the rules
     * were chosen for must hold: change a number in FrameWatch only with
     * these.
     */
    @Test
    fun noRecordingOfARealRunIsAccusedWhereverTheWatchBegins() {
        val recorded = recordings("frame_looks.tsv")
        assertTrue("the fixture has ${recorded.size} recordings", recorded.size >= 100)
        assertTrue(recorded.values.sumOf { it.size } >= 19_000)
        val measured = recordings("frame_looks_phones.tsv")
        assertTrue("the phones' fixture has ${measured.size} runs", measured.isNotEmpty())
        val all = recorded + measured
        var good = 0
        for ((name, looks) in all) {
            for (start in looks.indices) {
                val verdict = feed(FrameWatch(), 1, looks.subList(start, looks.size))
                assertFalse("$name from look $start: $verdict", verdict == FrameWatch.Verdict.NOISE || verdict == FrameWatch.Verdict.FLAT)
                if (verdict == FrameWatch.Verdict.GOOD) good++
            }
            // Speckle: the verdict wants 32 looks of 40; no recording has more than a quarter of that anywhere.
            val speckle = looks.map { if (it.kind == FrameWatch.Kind.SPECKLE) 1 else 0 }
            val worst = if (speckle.size < FrameWatch.NOISE_LOOKS) speckle.sum() else speckle.windowed(FrameWatch.NOISE_LOOKS).maxOf { it.sum() }
            assertTrue("$name: $worst speckle looks in twenty seconds", worst <= 8)
            // One colour: the verdict wants 120 looks in a row; none has 20.
            val flat = longestRun(looks, FrameWatch.Kind.FLAT)
            assertTrue("$name: one colour for $flat looks", flat < 20)
        }
        assertTrue(good > 15_000)
    }

    /** The player's screenshot (158 per thousand unlike their neighbours, 587 the ground's colour), which a recording never comes near for long. */
    @Test
    fun thePlayersScreenshotIsCaughtWhenItMovesAndNotWhenItStandsStill() {
        val moving = List(40) { Look(false, 158, 587, 158 * 158 / 1000, 60) }   // specks at random places: a sixth where one was
        assertEquals(FrameWatch.Verdict.NOISE, feed(FrameWatch(), 1, moving))
        // The same frame held: by the numbers a picture. What the watch cannot tell, it does not accuse.
        assertEquals(FrameWatch.Verdict.GOOD, feed(FrameWatch(), 1, List(40) { Look(false, 158, 587, 158, 60) }))
    }

    private fun choice(id: String, score: Int) = DriverCatalog.Choice(
        if (id == DriverCatalog.SYSTEM) null else DriverCatalog.Entry(
            id, id, "1", "turnip", "s", "https://github.com/o/r/releases/download/v/$id.zip", "", "ab".repeat(32), 1, "l.so", "MIT",
            true, 26, emptyList(), score, emptyList(), emptyList(), "",
        ),
        score, false,
    )

    @Test
    fun theNextDriverIsTheBestThatIsOnThePhone() {
        val ranked = listOf(choice("a", 100), choice("b", 80), choice("c", 60), choice("system", 40))
        assertEquals("b", DriverFallback.next(ranked, "a") { true }!!.id)
        // b is a download nobody has made: the next that is ready.
        assertEquals("c", DriverFallback.next(ranked, "a") { it.id != "b" }!!.id)
        assertEquals("system", DriverFallback.next(ranked, "a") { it.entry == null }!!.id)
        // The phone's own failed last: nothing is left.
        assertNull(DriverFallback.next(listOf(choice("system", 40)), "system") { true })
        assertNull(DriverFallback.next(emptyList(), "a") { true })
    }

    @Test
    fun theSelfTestsReportIsReadWholeOrCutShort() {
        val passed = DriverSelfTest.parse(
            "stage=open\ndriver=libvulkan_freedreno.so (adrenotools, the Android Vulkan HAL)\nstage=instance\nstage=device\n" +
                "device=Turnip Adreno (TM) 740\nvendor=0x5143\ndeviceId=0x43050A01\ndriverName=turnip Mesa driver\n" +
                "driverInfo=Mesa 27.0.0-devel\napi=1.4.335\nstage=draw\nstage=compare\nresult=pass\n",
            ended = true,
        )
        assertTrue(passed.passed)
        assertEquals("", passed.reason)
        assertEquals("Turnip Adreno (TM) 740", passed.device)
        assertEquals("0x43050A01", passed.deviceId)
        assertTrue(passed.line().startsWith("passed; Turnip Adreno (TM) 740 (vendor 0x5143 device 0x43050A01), turnip Mesa driver"))

        val refused = DriverSelfTest.parse(
            "stage=open\nstage=instance\nstage=device\ndevice=Adreno (TM) 616\n" +
                "result=fail:this graphics chip or its driver lacks descriptor indexing, which the renderer needs\n",
            ended = true,
        )
        assertFalse(refused.passed)
        assertEquals("this graphics chip or its driver lacks descriptor indexing, which the renderer needs", refused.reason)

        // The process died compiling the pipelines, or went silent there.
        val text = "stage=open\nstage=instance\nstage=device\ndevice=Adreno (TM) 830\nstage=shaders\nstage=pipelines\n"
        assertEquals("the driver crashed at compiling the pipelines", DriverSelfTest.parse(text, ended = true).reason)
        assertEquals("the driver hung at compiling the pipelines", DriverSelfTest.parse(text, ended = false).reason)
        assertFalse(DriverSelfTest.parse(text, ended = true).passed)
        // Nothing written at all.
        assertEquals("the test's process ended before it began", DriverSelfTest.parse("", ended = true).reason)
        assertEquals("the test did not start", DriverSelfTest.parse("", ended = false).reason)
        // A frame that came out wrong.
        val wrong = DriverSelfTest.parse("stage=compare\nresult=fail:the test frame is wrong: the BC1 texture came out #000000, not #00FF00\n", true)
        assertEquals("the test frame is wrong: the BC1 texture came out #000000, not #00FF00", wrong.reason)
    }

    @Test
    fun aCrashIsTheDriversWhenItsInnermostFramesAreInADriver() {
        // The Snapdragon 8 Elite player's crash, as Tombstone.read writes a backtrace.
        val qualcomm = "    signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault address 0x0\n    thread 1234 (pipelines):\n" +
            "      #00 pc 0000000000a1b2c3  /vendor/lib64/libllvm-qgl.so\n" +
            "      #01 pc 0000000000a1b2d4  /vendor/lib64/libllvm-qgl.so\n" +
            "      #02 pc 0000000000123456  /vendor/lib64/hw/vulkan.adreno.so\n" +
            "      #03 pc 0000000000345678  /data/app/x/lib/arm64/libaw.so (vk::pipeline::Create+120)\n"
        assertTrue(DriverFallback.crashInDriver(qualcomm, ""))
        // An imported driver, loaded from the app's own folders under whatever name its zip gave it.
        val imported = "    thread 1 (gpu):\n      #00 pc 00000000000aaaaa  /data/user/0/com.aw.recomp/files/drivers/turnip-27.0.0-r2/vulkan.ad07xx.so\n" +
            "      #01 pc 00000000000bbbbb  /data/app/x/lib/arm64/libaw.so\n"
        assertTrue(DriverFallback.crashInDriver(imported, "vulkan.ad07xx.so"))
        assertTrue(DriverFallback.crashInDriver("      #00 pc 0000000000000001  /somewhere/odd/vulkan.ad07xx.so\n", "vulkan.ad07xx.so"))
        // The game's own crash, with the driver far down the stack or not there at all.
        val ours = "    thread 9 (guest):\n" + (0 until 7).joinToString("") { "      #0$it pc 000000000000100$it  /data/app/x/lib/arm64/libaw.so (sub_8212$it+4)\n" } +
            "      #07 pc 0000000000123456  /vendor/lib64/hw/vulkan.adreno.so\n"
        assertFalse(DriverFallback.crashInDriver(ours, ""))
        assertFalse(DriverFallback.crashInDriver("    no signal recorded\n    (the crashing thread 5 is not in the tombstone)\n", "libvulkan_freedreno.so"))
        assertFalse(DriverFallback.crashInDriver("", ""))
    }
}
