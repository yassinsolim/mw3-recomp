package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/**
 * DriverRescue's reading of how a run ended, from players' real logs
 * (src/test/resources/driver_rescue/, the crashing thread's frames and the
 * run's start-up steps and fault report as the log share carried them):
 *
 *  - Homura (case C0009: Snapdragon 8 Elite, Adreno 830, Advanced Warfare
 *    0.1.6 on the phone's own driver V@0800.71): Qualcomm's shader compiler,
 *    libllvm-qgl.so, crashed under vkCreateGraphicsPipelines after start-up.
 *  - Shiota (case C0005: Snapdragon 710, Adreno 616, Android 11, MW3 0.1.12
 *    on an imported Turnip): no tombstone (Android 11 keeps none an app may
 *    read); the runtime's report has the fault at the vulkan-instance step,
 *    before any frame, in the runtime's own library.
 *  - The same Advanced Warfare log's other crash: the game's recompiled code
 *    (libawgame.so) on its Database thread, after start-up, which is not the
 *    driver's.
 */
class DriverRescueTest {
    private fun resource(name: String): String =
        javaClass.classLoader!!.getResourceAsStream("driver_rescue/$name")!!.readBytes().toString(Charsets.UTF_8)

    private val homuraTombstone = resource("homura_tombstone.trace")
    private val homuraLog = resource("homura_game.trace")
    private val shiotaLog = resource("shiota_game.trace")
    private val gameCodeTombstone = resource("aw_game_code_tombstone.trace")

    @Test
    fun homurasCrashIsInQualcommsShaderCompiler() {
        assertEquals("libllvm-qgl.so", DriverFallback.driverLibraryOf(homuraTombstone, ""))
        val run = DriverRescue.runLog(homuraLog, 23362)!!
        assertEquals("guest", run.lastStep)
        // The runtime's own report says the same where no tombstone was kept: its handler's frames are skipped.
        assertEquals("libllvm-qgl.so", DriverFallback.driverLibraryOf(run.backtrace, ""))
        assertEquals(DriverRescue.Verdict(DriverRescue.End.DRIVER_CRASH, "libllvm-qgl.so"),
            DriverRescue.classify(true, homuraTombstone, run, framed = false, library = ""))
        assertEquals(DriverRescue.Verdict(DriverRescue.End.DRIVER_CRASH, "libllvm-qgl.so"),
            DriverRescue.classify(true, null, run, framed = true, library = ""))
    }

    @Test
    fun shiotasCrashIsBeforeThePictureAtTheDriversStep() {
        val run = DriverRescue.runLog(shiotaLog, 23400)!!
        assertEquals("vulkan-instance", run.lastStep)
        assertEquals(true, run.faulted)
        // Its frames are the runtime's and the handler's: not a driver's file.
        assertNull(DriverFallback.driverLibraryOf(run.backtrace, "vulkan.ad07xx.so"))
        assertEquals(DriverRescue.Verdict(DriverRescue.End.NO_PICTURE),
            DriverRescue.classify(true, null, run, framed = false, library = "vulkan.ad07xx.so"))
        // Not known whether it framed (a run from before the app kept that): the step decides.
        assertEquals(DriverRescue.End.NO_PICTURE, DriverRescue.classify(true, null, run, null, "").end)
        // Another pid's run is not this one.
        assertNull(DriverRescue.runLog(shiotaLog, 23401))
    }

    @Test
    fun aCrashInTheGamesCodeDoesNotCount() {
        assertNull(DriverFallback.driverLibraryOf(gameCodeTombstone, ""))
        val run = DriverRescue.RunLog("guest", true, gameCodeTombstone)
        assertEquals(DriverRescue.End.OTHER, DriverRescue.classify(true, gameCodeTombstone, run, framed = false, library = "").end)
        assertEquals(DriverRescue.End.OTHER, DriverRescue.classify(true, gameCodeTombstone, null, framed = false, library = "").end)
        // A run that ended without a crash says nothing against its driver, whatever step it was at.
        assertEquals(DriverRescue.End.OTHER,
            DriverRescue.classify(false, null, DriverRescue.RunLog("vulkan-device", false, ""), framed = false, library = "").end)
    }

    @Test
    fun runsAreCountedInARowOnOneDriver() {
        val crash = DriverRescue.Verdict(DriverRescue.End.DRIVER_CRASH, "libllvm-qgl.so")
        val dark = DriverRescue.Verdict(DriverRescue.End.NO_PICTURE)
        val fine = DriverRescue.Verdict(DriverRescue.End.OTHER)
        var streak = DriverRescue.Streak.NONE.after("system", crash)
        assertEquals(1, streak.count)
        streak = streak.after("system", crash)
        assertEquals(DriverRescue.Streak("system", 2, 0, "libllvm-qgl.so"), streak)
        assertEquals("the game crashed inside it (libllvm-qgl.so) on the last 2 starts", streak.why())
        // Kept as text in the preferences, and read back the same.
        assertEquals(streak, DriverRescue.Streak.parse(streak.toString()))
        // A run that ended well starts again; so does a run on another driver.
        assertEquals(DriverRescue.Streak.NONE, streak.after("system", fine))
        assertEquals(DriverRescue.Streak("dir:Turnip", 0, 1, ""), streak.after("dir:Turnip", dark))
        assertEquals(DriverRescue.Streak.NONE, DriverRescue.Streak.parse(DriverRescue.Streak.NONE.toString()))
        assertEquals(DriverRescue.Streak.NONE, DriverRescue.Streak.parse("garbage"))
    }

    @Test
    fun keepMineIsForOneVersion() {
        val gl830 = "Adreno (TM) 830 (Qualcomm), OpenGL ES 3.2 V@0800.71 (GIT@208ca19915, Iab05a79315, 1770369230) (Date:02/06/26); Vulkan 1.3.0 declared"
        val newer = gl830.replace("V@0800.71", "V@0842.6")
        assertEquals(DriverRescue.keepKey("system", null, gl830), DriverRescue.keepKey("system", null, gl830))
        assertNotEquals(DriverRescue.keepKey("system", null, gl830), DriverRescue.keepKey("system", null, newer))
        val r2 = DriverStore.Driver(File("/d/a"), "Turnip", "Mesa", "x", "27.0.0-R2", "vulkan.ad07xx.so", "", 30)
        assertNotEquals(DriverRescue.keepKey("dir:a", r2, gl830), DriverRescue.keepKey("dir:a", r2.copy(version = "27.0.0-R3"), gl830))
    }

    @Test
    fun theCatalogCallsHomurasDriverBrokenForAwOnly() {
        val catalog = DriverCatalog.parse(File("../drivers/catalog.json").readText())!!
        val gl830 = "Adreno (TM) 830 (Qualcomm), OpenGL ES 3.2 V@0800.71 (GIT@208ca19915, Iab05a79315, 1770369230) (Date:02/06/26); Vulkan 1.3.0 declared"
        val gpu = Gpu.parse(gl830)
        assertNotNull(catalog.brokenHere(DriverCatalog.SYSTEM, gpu, "aw", gl830))
        assertNull(catalog.brokenHere(DriverCatalog.SYSTEM, gpu, "mw3", gl830))
        assertNull(catalog.brokenHere(DriverCatalog.SYSTEM, gpu, "aw", gl830.replace("V@0800.71", "V@0842.6")))
        assertNull(catalog.brokenHere("turnip-27.0.0-20261121-a8xx-banner", gpu, "aw", gl830))
        assertNull(catalog.brokenHere("dir:imported", gpu, "aw", gl830))
        // What he is offered: the A8xx Turnip the apps carry.
        val best = catalog.ranked(gpu, 36, "aw", gl830, setOf(DriverCatalog.SYSTEM)).first()
        assertEquals("turnip-27.0.0-20261121-a8xx-banner", best.id)
    }
}
