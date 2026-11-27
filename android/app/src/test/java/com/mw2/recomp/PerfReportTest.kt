package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A performance report holds the fields PRIVACY.md lists and nothing else,
 * whatever the run or the phone hands the builder, and nothing in it is
 * shaped like a path (PerfReports.kt, docs/perf-reports.md).
 */
class PerfReportTest {
    private val install = "0123456789abcdef0123456789abcdef"
    private val phone = mapOf("model" to "Retroid Pocket Flip 2", "chip" to "QTI SM8250, board kona", "android" to "13", "app" to "0.1.7 (local)")
    private val kept = mapOf(
        "seconds" to "431", "fps" to "58.44", "low1" to "41.03", "limit" to "cpu", "map" to "trainer",
        "gpu" to "Turnip Adreno (TM) 650", "driver" to "turnip Mesa driver", "driver_version" to "Mesa 25.3.0-devel (git-0a1b2c3)",
        "game" to "mw2", "game_version" to "disc", "mode" to "campaign", "preset" to "none", "scene_scale" to "70",
        "shadows" to "auto", "fps_limit" to "60", "multiplier" to "1", "thermal" to "1",
    )

    @Test
    fun aReportHoldsTheListedFieldsAndNoOther() {
        // What a run or a later version of the app might hand over besides.
        val more = kept + mapOf(
            "path" to "/storage/emulated/0/Android/data/com.mw2.recomp/files/game", "player" to "Someone",
            "disc" to "MW2.iso", "sha256" to "ab".repeat(32), "address" to "203.0.113.7",
        )
        val report = PerfReport.build(install, phone + mapOf("serial" to "R5CT1234", "account" to "someone@example.com"), more)!!
        assertEquals(PerfReport.FIELDS, PerfReport.fields(report).map { it.first })
        assertEquals(
            """{"v":1,"install":"0123456789abcdef0123456789abcdef","model":"Retroid Pocket Flip 2",""" +
                """"chip":"QTI SM8250, board kona","gpu":"Turnip Adreno (TM) 650","driver":"turnip Mesa driver",""" +
                """"driver_version":"Mesa 25.3.0-devel (git-0a1b2c3)","android":"13","app":"0.1.7 (local)","game":"mw2",""" +
                """"game_version":"disc","mode":"campaign","map":"trainer","minutes":7,"fps":58.4,"low1":41.0,""" +
                """"limit":"cpu","preset":"none","scene_scale":70,"shadows":"auto","fps_limit":60,"multiplier":1,"thermal":1}""",
            report,
        )
        for (secret in listOf("storage", "Someone", "iso", "abab", "203.0.113.7", "R5CT1234", "example.com")) {
            assertFalse(secret, report.contains(secret))
        }
        assertTrue("under the server's 2048 bytes", report.length < 1024)
    }

    @Test
    fun nothingShapedLikeAPathLeaves() {
        val report = PerfReport.build(
            install,
            phone + mapOf("model" to "/sdcard/Download/My \"Phone\"\\x"),
            kept + mapOf(
                "map" to "../../zone/english/mp_dome.ff", "gpu" to "C:\\Users\\someone\\driver.so",
                "driver_version" to "x".repeat(300), "mode" to "/etc/passwd", "limit" to "{\"a\":1}",
                "scene_scale" to "70/../x", "thermal" to "99",
            ),
        )!!
        assertFalse(report, report.contains('/') || report.contains('\\'))
        val fields = PerfReport.fields(report).toMap()
        assertEquals(PerfReport.FIELDS.size, fields.size)
        assertEquals("a map that is not a map's name is left out", "", fields["map"])
        assertEquals("sdcard Download My Phone x", fields["model"])
        assertEquals("C: Users someone driver.so", fields["gpu"])
        assertEquals(64, fields["driver_version"]!!.length)
        assertEquals("campaign", fields["mode"])
        assertEquals("unknown", fields["limit"])
        assertEquals("100", fields["scene_scale"])
        assertEquals("6", fields["thermal"])
    }

    @Test
    fun aSessionTooShortOrWithoutNumbersIsNoReport() {
        assertNull(PerfReport.build(install, phone, kept + ("seconds" to "59")))
        assertNull(PerfReport.build(install, phone, kept + ("fps" to "0.0")))
        assertNull(PerfReport.build(install, phone, kept - "low1"))
        assertNull(PerfReport.build(install, phone, emptyMap()))
        assertEquals("1", PerfReport.fields(PerfReport.build(install, phone, kept + ("seconds" to "60"))!!).toMap()["minutes"])
    }

    @Test
    fun theRunsLinesAreReadAsTheyWereKept() {
        val lines = PerfReport.lines("seconds=431\nfps=58.4\nmap=trainer\ndriver_version=Mesa 25.3.0 (a=b)\n\nnothing\n")
        assertEquals(mapOf("seconds" to "431", "fps" to "58.4", "map" to "trainer", "driver_version" to "Mesa 25.3.0 (a=b)"), lines)
    }

    @Test
    fun theInstallNumberIsNewEachMonth() {
        var made = 0
        val fresh = { made++; "f".repeat(32) }
        assertEquals(install, PerfReport.install(install, "2026-11", "2026-11", fresh))
        assertEquals(0, made)
        assertNotEquals(install, PerfReport.install(install, "2026-11", "2026-12", fresh))
        assertEquals("f".repeat(32), PerfReport.install(null, null, "2026-11", fresh))
        assertEquals("one that is not a number of ours is not sent", "f".repeat(32), PerfReport.install("R5CT1234", "2026-11", "2026-11", fresh))
        assertEquals(3, made)
    }

    @Test
    fun theExampleIsAReportAndReadsAsOne() {
        assertEquals(PerfReport.FIELDS, PerfReport.fields(PerfReport.EXAMPLE).map { it.first })
        val read = PerfReport.readable(PerfReport.EXAMPLE)
        assertEquals("every field but the format's version has a line", PerfReport.FIELDS.size - 1, read.lines().size)
        assertTrue(read, read.contains("Map: mp_dome") && read.contains("Average FPS: 58.6") && read.contains("Phone: AYN Thor"))
    }
}
