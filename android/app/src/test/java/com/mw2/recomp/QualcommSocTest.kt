package com.mw2.recomp

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * QualcommSoc.isQualcomm with what real phones report: Build.HARDWARE,
 * Build.BOARD, Build.SOC_MANUFACTURER where it exists, and the GPU line
 * DeviceInfo.gpu writes (from players' logs where there is one, otherwise
 * from the phones' own build.prop and fstab.<ro.hardware>).
 */
class QualcommSocTest {

    private fun never(): String = throw AssertionError("the GPU is not asked when the system's answer suffices")

    @Test
    fun realme5ProOnAndroid11() {
        // A player's log, MW3 0.1.12: "phone: realme RMX1971 (RMX1971), Android 11 (API 30)".
        val gpu = "Adreno (TM) 616 (Qualcomm), OpenGL ES 3.2 V@0502.0 (GIT@5af5ff1, Iff02ff076a, 1609853540) (Date:01/05/21); Vulkan 1.1.0 declared"
        assertTrue(QualcommSoc.isQualcomm(30, null, "qcom", "sdm710") { gpu })
        // The board alone, and the GPU alone, say it too.
        assertTrue(QualcommSoc.isQualcomm(30, null, "RMX1971", "sdm710") { "" })
        assertTrue(QualcommSoc.isQualcomm(30, null, "RMX1971", "RMX1971") { gpu })
    }

    @Test
    fun pixel4aOnAndroid10NamesOnlyItsGpu() {
        // Google's Snapdragon 730G phone: ro.hardware and ro.product.board are its codename.
        assertTrue(QualcommSoc.isQualcomm(29, null, "sunfish", "sunfish") { "Adreno (TM) 618 (Qualcomm), OpenGL ES 3.2 V@0490.0" })
    }

    @Test
    fun snapdragonCodenameBoards() {
        // Samsung's Snapdragon models name the platform as the board.
        assertTrue(QualcommSoc.isQualcomm(30, null, "unknown", "msmnile") { "" })
        assertTrue(QualcommSoc.isQualcomm(29, null, "unknown", "atoll") { "" })
        assertTrue(QualcommSoc.isQualcomm(30, null, "unknown", "sm6150") { "" })
    }

    @Test
    fun aynThorAndRetroidPocketFlip2AreDecidedBySocManufacturer() {
        // Android 13, both: "QTI" from Build.SOC_MANUFACTURER, as before this rule.
        assertTrue(QualcommSoc.isQualcomm(33, "QTI", "qcom", "kalama") { never() })
        assertTrue(QualcommSoc.isQualcomm(33, "QTI", "qcom", "kona") { never() })
        // The Retroid Pocket 5 of a player's log: "chip: QTI SM8250, hardware qcom, board kona".
        assertTrue(QualcommSoc.isQualcomm(33, "QTI", "qcom", "kona") { never() })
    }

    @Test
    fun androidTwelveAndLaterKeepTheSystemsWord() {
        assertFalse(QualcommSoc.isQualcomm(33, "Mediatek", "mt6893", "mt6893") { never() })
        assertFalse(QualcommSoc.isQualcomm(31, "Samsung", "exynos2100", "exynos2100") { never() })
    }

    @Test
    fun dimensity700OnAndroid11IsNot() {
        // Poco M3 Pro 5G (camellian), MIUI 12.5 on Android 11: ro.product.board=camellia,
        // ro.board.platform=mt6833, fstab.mt6833 (ro.hardware=mt6833); Mali-G57 MC2.
        assertFalse(QualcommSoc.isQualcomm(30, null, "mt6833", "camellia") { "Mali-G57 MC2 (ARM), OpenGL ES 3.2 v1.r26p0-01eac0.2f7d3be5f5bd6b5ab1ceb4ef1ff5e36c" })
    }

    @Test
    fun exynos9611IsNot() {
        // Galaxy A51 (SM-A515F): ro.product.board=exynos9611, ro.board.platform=universal9611,
        // fstab.exynos9611 (ro.hardware=exynos9611); Mali-G72 MP3.
        assertFalse(QualcommSoc.isQualcomm(30, null, "exynos9611", "exynos9611") { "Mali-G72 (ARM), OpenGL ES 3.2 v1.r25p1" })
        assertFalse(QualcommSoc.isQualcomm(29, null, "exynos9611", "universal9611") { "Mali-G72 (ARM), OpenGL ES 3.2 v1.r25p1" })
    }

    @Test
    fun otherChipsAreNot() {
        // Unisoc T618, PowerVR Helio, Kirin: their boards and GPUs.
        assertFalse(QualcommSoc.isQualcomm(30, null, "ums512", "ums512_1h10") { "Mali-G52 (ARM), OpenGL ES 3.2" })
        assertFalse(QualcommSoc.isQualcomm(29, null, "mt6765", "k65v1_64_bsp") { "PowerVR Rogue GE8320 (Imagination Technologies), OpenGL ES 3.2" })
        assertFalse(QualcommSoc.isQualcomm(29, null, "kirin980", "ELE") { "Mali-G76 (ARM), OpenGL ES 3.2" })
    }

    @Test
    fun aGpuThatCannotBeAskedIsNotQualcomm() {
        assertFalse(QualcommSoc.isQualcomm(30, null, "mt6833", "camellia") { throw IllegalStateException("no EGL") })
        assertFalse(QualcommSoc.isQualcomm(30, null, "", "") { "OpenGL ES would not say; no Vulkan declared" })
    }
}
