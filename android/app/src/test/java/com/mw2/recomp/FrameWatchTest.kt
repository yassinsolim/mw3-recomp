package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The watch over a run's first frames (FrameWatch.kt), on looks as the
 * runtime hands them over twice a second; which driver comes after one that
 * failed; what the self-test's report file says, whole or cut short; and
 * whether a crash was inside the driver.
 */
class FrameWatchTest {

    /** [looks] of (flat, isolated), half a second apart from [from]; returns the verdict after the last. */
    private fun feed(watch: FrameWatch, from: Int, looks: List<Pair<Boolean, Int>>): FrameWatch.Verdict {
        var verdict = watch.verdict
        looks.forEachIndexed { index, (flat, isolated) ->
            val serial = from + index
            verdict = watch.look(serial * 500L, serial, flat, isolated)
        }
        return verdict
    }

    private val black = true to 0
    private val picture = false to 30
    private val noise = false to 400

    @Test
    fun aHealthyStartIsBlackThenAPicture() {
        val watch = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(40) { black }))
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 41, List(5) { picture }))
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 46, listOf(picture)))
        // Good is final: noise afterwards is the game's own business.
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 47, List(200) { noise }))
    }

    @Test
    fun noiseForTwentySecondsIsNoise() {
        val watch = FrameWatch()
        // Nineteen and a half seconds of it: not yet.
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(40) { noise }))
        assertEquals(FrameWatch.Verdict.NOISE, feed(watch, 41, listOf(noise)))
    }

    @Test
    fun theOddFrameThatMeasuresAsAPictureDoesNotHideNoise() {
        val watch = FrameWatch()
        // One look in six is under the line, never six in a row.
        val looks = List(48) { if (it % 6 == 5) picture else noise }
        assertEquals(FrameWatch.Verdict.NOISE, feed(watch, 1, looks))
    }

    @Test
    fun aBusySceneAmongPicturesIsNotNoise() {
        val watch = FrameWatch()
        // A black load, then gameplay whose busiest frames measure as noise would.
        val looks = List(10) { black } + List(60) { if (it % 3 == 0) noise else picture }
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, looks.take(40)))
        assertFalse(feed(watch, 41, looks.drop(40)) == FrameWatch.Verdict.NOISE)
    }

    @Test
    fun oneColourForAMinuteIsFlatAndNotBefore() {
        val watch = FrameWatch()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 1, List(120) { black }))
        assertEquals(FrameWatch.Verdict.FLAT, feed(watch, 121, listOf(black)))
        // The player waits on: the minute starts over.
        watch.startOver()
        assertEquals(FrameWatch.Verdict.WATCHING, feed(watch, 122, List(100) { black }))
        assertEquals(FrameWatch.Verdict.GOOD, feed(watch, 222, List(6) { picture }))
    }

    @Test
    fun aLookIsCountedOnceAndNoneIsNothing() {
        val watch = FrameWatch()
        // No frame looked at yet (serial 0), for minutes: the black-screen watch's business, not this one's.
        repeat(300) { assertEquals(FrameWatch.Verdict.WATCHING, watch.look(it * 500L, 0, true, 0)) }
        // The same look read sixty times is one look.
        repeat(60) { assertEquals(FrameWatch.Verdict.WATCHING, watch.look(200_000L + it * 500L, 1, false, 10)) }
        // The line itself is noise.
        val edge = FrameWatch()
        assertEquals(FrameWatch.Verdict.NOISE, feed(edge, 1, List(41) { false to FrameWatch.NOISE_AT }))
        val under = FrameWatch()
        assertEquals(FrameWatch.Verdict.GOOD, feed(under, 1, List(6) { false to FrameWatch.NOISE_AT - 1 }))
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
