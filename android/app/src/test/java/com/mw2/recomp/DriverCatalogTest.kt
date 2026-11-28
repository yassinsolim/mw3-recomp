package com.mw2.recomp

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The driver catalog as the app reads it (DriverCatalog.kt): the repository's
 * own android/drivers/catalog.json with the GPU lines DeviceInfo.gpu writes
 * on real phones, and catalogs made here for the rules that file has no case
 * of.
 */
class DriverCatalogTest {

    private val catalog = DriverCatalog.parse(File("../drivers/catalog.json").readText())!!

    private val thor = "Adreno (TM) 740 (Qualcomm), OpenGL ES 3.2 V@0676.42; Vulkan 1.3.0 declared"
    private val flip = "Adreno (TM) 650 (Qualcomm), OpenGL ES 3.2 V@0615.65; Vulkan 1.1.0 declared"
    private val elite = "Adreno (TM) 830 (Qualcomm), OpenGL ES 3.2 V@0800.71 (GIT@208ca19915, Iab05a79315, 1770369230) (Date:02/06/26); Vulkan 1.3.0 declared"

    private fun ids(gpu: String, sdk: Int, game: String, failed: Set<String> = emptySet()) =
        catalog.ranked(Gpu.parse(gpu), sdk, game, gpu, failed).map { it.id }

    @Test
    fun theChipIsReadFromTheOpenGlLine() {
        assertEquals(Gpu(Gpu.ADRENO, 740, "Adreno 740"), Gpu.parse(thor))
        assertEquals(Gpu.ADRENO to 616, Gpu.parse("Adreno (TM) 616 (Qualcomm), OpenGL ES 3.2 V@0502.0").let { it.vendor to it.model })
        assertEquals(Gpu.MALI to 715, Gpu.parse("Mali-G715 (ARM), OpenGL ES 3.2 v1.r44p0").let { it.vendor to it.model })
        assertEquals(Gpu.MALI to 720, Gpu.parse("Immortalis-G720 (ARM), OpenGL ES 3.2").let { it.vendor to it.model })
        assertEquals(Gpu.XCLIPSE to 940, Gpu.parse("ANGLE (Samsung Xclipse 940) on Vulkan 1.3.279").let { it.vendor to it.model })
        assertEquals(Gpu.POWERVR, Gpu.parse("PowerVR Rogue GE8320 (Imagination Technologies)").vendor)
        // An Adreno the line does not number is offered nothing: model 0 is in no driver's range.
        assertEquals(Gpu.ADRENO to 0, Gpu.parse("Adreno (TM) 7c+ Gen 3 (Qualcomm)").let { it.vendor to it.model })
        assertEquals(Gpu.OTHER, Gpu.parse("OpenGL ES would not say; no Vulkan declared").vendor)
    }

    @Test
    fun theRepositorysCatalogIsWhole() {
        assertTrue(catalog.version > 0)
        assertTrue(catalog.drivers.size >= 8)
        for (driver in catalog.drivers) {
            assertTrue(driver.id, driver.sha256.matches(Regex("[0-9a-f]{64}")))
            assertTrue(driver.id, driver.url.isEmpty() || DriverCatalog.allowedUrl(driver.url))
            assertTrue(driver.id, driver.url.isNotEmpty() || driver.asset.isNotEmpty())
            // Qualcomm's own drivers are linked to: never the app's choice, never carried.
            if (driver.kind == "qualcomm") assertTrue(driver.id, !driver.auto && driver.asset.isEmpty())
        }
        // The two the apps carry are the ones DriverStore unpacks.
        assertEquals(DriverStore.BUNDLED_ASSET, catalog.entry("turnip-27.0.0-r2")!!.asset)
        assertEquals(DriverStore.BUNDLED_ASSET_API29, catalog.entry("turnip-27.0.0-483aff3-api29")!!.asset)
    }

    @Test
    fun theTwoTestPhonesAreGivenTheTurnipTheyWereMeasuredWith() {
        for (game in listOf("mw3", "mw2", "cod4", "aw", "bo3")) {
            assertEquals(game, "turnip-27.0.0-r2", ids(thor, 33, game).first())
            assertEquals(game, "turnip-27.0.0-r2", ids(flip, 33, game).first())
        }
        // Then the project's own, known good there too, then the rest by rank, the phone's own after every Turnip.
        val order = ids(flip, 33, "aw")
        assertEquals(listOf("turnip-27.0.0-r2", "turnip-27.0.0-483aff3-api29", "turnip-27.0.0-r3"), order.take(3))
        assertTrue(order.indexOf("system") > order.indexOf("turnip-25.3.0-bbdd688-weabchan"))
        // Nothing the app may not choose by itself is in the list.
        assertFalse(order.any { it.startsWith("qualcomm-") || it.contains("draft") })
    }

    @Test
    fun whatFailedOnThePhoneIsLeftOutAndTheNextIsFirst() {
        assertEquals("turnip-27.0.0-483aff3-api29", ids(thor, 33, "mw3", setOf("turnip-27.0.0-r2")).first())
        val left = ids(thor, 33, "mw3", setOf("turnip-27.0.0-r2", "turnip-27.0.0-483aff3-api29", "turnip-27.0.0-r3"))
        assertEquals("turnip-stable-26.2.3-v2", left.first())
        // The phone's own can fail too, and is then not offered.
        assertFalse("system" in ids(thor, 33, "mw3", setOf("system")))
    }

    @Test
    fun android10IsOnlyOfferedWhatLoadsThere() {
        // Every community Turnip asks for Android 11's libc: only the project's build is left, then the phone's own.
        assertEquals(listOf("turnip-27.0.0-483aff3-api29", "system"), ids(flip, 29, "mw3"))
        assertEquals(listOf("system"), ids(flip, 28, "mw3"))
    }

    @Test
    fun aSnapdragon8EliteGetsATurnipWithTheA8xxPatchesAndNotItsOwnDriverInAdvancedWarfare() {
        val aw = ids(elite, 36, "aw")
        assertEquals("turnip-27.0.0-20261121-a8xx-banner", aw.first())
        // R2 and the project's unpatched build are for the 6xx and 7xx; Qualcomm's V@0800.71 crashed in its shader
        // compiler on a player's phone.
        assertFalse("turnip-27.0.0-r2" in aw)
        assertFalse("turnip-27.0.0-483aff3-api29" in aw)
        assertFalse("system" in aw)
        // The same phone in another game, and another version of that driver, keep the phone's own as the last resort.
        assertEquals("system", ids(elite, 36, "mw3").last())
        assertEquals("system", ids(elite.replace("V@0800.71", "V@0842.6"), 36, "aw").last())
    }

    @Test
    fun aChipThatIsNoAdrenoKeepsItsOwnDriver() {
        assertEquals(listOf("system"), ids("Mali-G715 (ARM), OpenGL ES 3.2 v1.r44p0", 34, "mw3"))
        assertEquals(listOf("system"), ids("ANGLE (Samsung Xclipse 940) on Vulkan 1.3.279", 34, "mw3"))
        assertEquals(listOf("system"), ids("OpenGL ES would not say; no Vulkan declared", 34, "mw3"))
        // PanVK is in the catalog for a Mali, for a player to ask for by name.
        val mali = Gpu.parse("Mali-G77 (ARM)")
        assertTrue(catalog.drivers.any { it.kind == "panvk" && it.fits(mali, 33) && !it.auto })
    }

    private fun one(extra: String) = """{"schema":1,"version":5,"drivers":[{"id":"abc-1","name":"A","version":"1","kind":"turnip",
        "source":"s","library":"lib.so","licence":"MIT","auto":true,"minApi":30,"rank":10,
        "gpus":[{"vendor":"adreno","from":600,"to":799}],$extra}],"system":[]}"""

    private val hash = "\"sha256\":\"" + "ab".repeat(32) + "\""

    @Test
    fun anEntryThatIsNotWholeIsLeftOut() {
        val good = one("$hash,\"url\":\"https://github.com/o/r/releases/download/v1/a.zip\"")
        assertEquals(1, DriverCatalog.parse(good)!!.drivers.size)
        // No hash, a short one, upper case.
        assertEquals(0, DriverCatalog.parse(one("\"url\":\"https://github.com/o/r/releases/download/v1/a.zip\""))!!.drivers.size)
        assertEquals(0, DriverCatalog.parse(good.replace("ab".repeat(32), "ab".repeat(31)))!!.drivers.size)
        assertEquals(0, DriverCatalog.parse(good.replace("ab".repeat(32), "AB".repeat(32)))!!.drivers.size)
        // A URL that is not a GitHub release asset over HTTPS.
        for (url in listOf(
            "http://github.com/o/r/releases/download/v1/a.zip", "https://example.com/a.zip",
            "https://github.com.evil.example/o/r/releases/download/v1/a.zip", "https://github.com/o/r/archive/main.zip",
            "https://github.com/o/r/releases/download/v1/a.zip?x=1", "file:///sdcard/a.zip",
        )) {
            assertFalse(url, DriverCatalog.allowedUrl(url))
            assertEquals(url, 0, DriverCatalog.parse(one("$hash,\"url\":\"$url\""))!!.drivers.size)
        }
        // Neither published nor carried; an asset outside drivers/; a library that is a path.
        assertEquals(0, DriverCatalog.parse(one(hash))!!.drivers.size)
        assertEquals(0, DriverCatalog.parse(one("$hash,\"asset\":\"../x.zip\""))!!.drivers.size)
        assertEquals(0, DriverCatalog.parse(good.replace("lib.so", "../lib.so"))!!.drivers.size)
        // A proprietary driver marked for the app's own choice.
        assertEquals(0, DriverCatalog.parse(good.replace("\"turnip\"", "\"qualcomm\""))!!.drivers.size)
        assertEquals(1, DriverCatalog.parse(good.replace("\"turnip\"", "\"qualcomm\"").replace("\"auto\":true", "\"auto\":false"))!!.drivers.size)
    }

    @Test
    fun whatIsNotACatalogIsNone() {
        assertNull(DriverCatalog.parse(""))
        assertNull(DriverCatalog.parse("<html>Not Found</html>"))
        assertNull(DriverCatalog.parse("{\"schema\":2,\"version\":9,\"drivers\":[]}"))
        assertNull(DriverCatalog.parse("{\"schema\":1,\"version\":0,\"drivers\":[]}"))
        assertNotNull(DriverCatalog.parse("{\"schema\":1,\"version\":1}"))
    }

    @Test
    fun aReportOutranksARankAndABrokenOneRemoves() {
        val text = """{"schema":1,"version":1,"drivers":[
            {"id":"high","name":"H","version":"1","kind":"turnip","source":"s","library":"l.so","licence":"MIT","auto":true,"minApi":26,"rank":90,
             "gpus":[{"vendor":"adreno","from":600,"to":799}],$hash,"url":"https://github.com/o/r/releases/download/v1/h.zip",
             "reports":[{"gpus":[{"vendor":"adreno","from":730,"to":730}],"game":"aw","result":"broken","source":"a log"}]},
            {"id":"low","name":"L","version":"1","kind":"turnip","source":"s","library":"l.so","licence":"MIT","auto":true,"minApi":26,"rank":20,
             "gpus":[{"vendor":"adreno","from":600,"to":799}],$hash,"url":"https://github.com/o/r/releases/download/v1/l.zip",
             "reports":[{"game":"mw3","result":"good","source":"a test"}]},
            {"id":"same","name":"S","version":"1","kind":"turnip","source":"s","library":"l.so","licence":"MIT","auto":true,"minApi":26,"rank":20,
             "gpus":[{"vendor":"adreno","from":600,"to":799}],$hash,"url":"https://github.com/o/r/releases/download/v1/s.zip"}],
            "system":[{"gpus":[{"vendor":"adreno","from":0,"to":9999}],"rank":40}]}"""
        val made = DriverCatalog.parse(text)!!
        val on = { model: Int, game: String -> made.ranked(Gpu(Gpu.ADRENO, model, ""), 33, game, "", emptySet()).map { it.id } }
        // Known good for the game first, whatever its rank; equal ranks as the catalog lists them.
        assertEquals(listOf("low", "high", "system", "same"), on(740, "mw3"))
        assertEquals(listOf("high", "system", "low", "same"), on(740, "cod4"))
        // Broken on the 730 in Advanced Warfare, and only there.
        assertEquals(listOf("system", "low", "same"), on(730, "aw"))
        assertEquals(listOf("high", "system", "low", "same"), on(740, "aw"))
        assertTrue(made.ranked(Gpu(Gpu.ADRENO, 740, ""), 33, "mw3", "", emptySet()).first().knownGood)
    }

    @Test
    fun aPackagesHashIsItsSha256() {
        // FIPS 180-2's "abc".
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            DriverStore.sha256("abc".byteInputStream()),
        )
        assertEquals("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", DriverStore.sha256(ByteArray(0).inputStream()))
        // A package is the catalog's only by that hash, in lower case as the catalog writes it.
        val r2 = catalog.entry("turnip-27.0.0-r2")!!
        assertEquals(r2, catalog.bySha(r2.sha256))
        assertEquals(r2, catalog.bySha(r2.sha256.uppercase()))
        assertNull(catalog.bySha("00".repeat(32)))
    }

    @Test
    fun aLinkIsFoundInWhatAnotherAppShared() {
        assertEquals("https://github.com/o/r/releases/download/v1/a.zip", DriverFetch.linkIn("https://github.com/o/r/releases/download/v1/a.zip"))
        assertEquals(
            "https://cdn.discordapp.com/attachments/1/2/turnip.zip?ex=abc&is=def",
            DriverFetch.linkIn("try this one https://cdn.discordapp.com/attachments/1/2/turnip.zip?ex=abc&is=def, works on my 8 gen 2"),
        )
        assertEquals("https://example.com/a.zip", DriverFetch.linkIn("(https://example.com/a.zip)"))
        assertNull(DriverFetch.linkIn("http://example.com/a.zip"))
        assertNull(DriverFetch.linkIn("no link here"))
    }
}
