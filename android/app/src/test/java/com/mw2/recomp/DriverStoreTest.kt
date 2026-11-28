package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * DriverStore.bundledAssetFor with the GPU lines DeviceInfo.gpu writes on
 * real phones (players' logs).
 */
class DriverStoreTest {

    private fun never(): String = throw AssertionError("the GPU is not asked below API 30")

    @Test
    fun snapdragon8EliteGetsTheA8xxTurnip() {
        // A player's log, AW 0.1.5: "phone: Xiaomi 24129PN74G (dada), Android 16 (API 36)",
        // "chip: QTI SM8750, hardware qcom, board sun"; Qualcomm's driver crashed in libllvm-qgl.so.
        val gpu = "Adreno (TM) 830 (Qualcomm), OpenGL ES 3.2 V@0800.71 (GIT@208ca19915, Iab05a79315, 1770369230) (Date:02/06/26); Vulkan 1.3.0 declared"
        assertTrue(QualcommSoc.isQualcomm(36, "QTI", "qcom", "sun") { never() })
        assertEquals(DriverStore.BUNDLED_ASSET_A8XX, DriverStore.bundledAssetFor(36) { gpu })
        assertEquals(DriverStore.BUNDLED_ASSET_A8XX, DriverStore.bundledAssetFor(35) { "Adreno (TM) 840 (Qualcomm), OpenGL ES 3.2 V@0846.0" })
        assertEquals(DriverStore.BUNDLED_ASSET_A8XX, DriverStore.bundledAssetFor(30) { "Adreno (TM) 810 (Qualcomm)" })
    }

    @Test
    fun adreno6xxAnd7xxKeepTheCommunityTurnip() {
        // AYN Thor (Adreno 740) and Retroid Pocket Flip 2 (Adreno 650), Android 13.
        assertEquals(DriverStore.BUNDLED_ASSET, DriverStore.bundledAssetFor(33) { "Adreno (TM) 740 (Qualcomm), OpenGL ES 3.2 V@0676.42" })
        assertEquals(DriverStore.BUNDLED_ASSET, DriverStore.bundledAssetFor(33) { "Adreno (TM) 650 (Qualcomm), OpenGL ES 3.2 V@0615.65" })
        // Realme 5 Pro, Android 11 (MW3 0.1.12's log).
        assertEquals(DriverStore.BUNDLED_ASSET, DriverStore.bundledAssetFor(30) { "Adreno (TM) 616 (Qualcomm), OpenGL ES 3.2 V@0502.0 (GIT@5af5ff1, Iff02ff076a, 1609853540) (Date:01/05/21); Vulkan 1.1.0 declared" })
    }

    @Test
    fun android10GetsTheProjectsMesaWithoutAskingTheGpu() {
        assertEquals(DriverStore.BUNDLED_ASSET_API29, DriverStore.bundledAssetFor(29) { never() })
    }

    @Test
    fun theThreePackagesAreDistinct() {
        val assets = setOf(DriverStore.BUNDLED_ASSET, DriverStore.BUNDLED_ASSET_A8XX, DriverStore.BUNDLED_ASSET_API29)
        assertEquals(3, assets.size)
    }

    @Test
    fun onlyAnAdreno8xxCounts() {
        assertTrue(DriverStore.adreno8xx("Adreno (TM) 810 (Qualcomm)"))
        assertTrue(DriverStore.adreno8xx("Adreno (TM) 830 (Qualcomm), OpenGL ES 3.2"))
        // The 8cx's and 7c's names are no 8xx, nor a renderer that says nothing.
        assertFalse(DriverStore.adreno8xx("Adreno 8c Gen 3"))
        assertFalse(DriverStore.adreno8xx("Adreno (TM) 7c+ Gen 3"))
        assertFalse(DriverStore.adreno8xx("Adreno (TM) 8000"))
        assertFalse(DriverStore.adreno8xx("OpenGL ES would not say; no Vulkan declared"))
        assertFalse(DriverStore.adreno8xx("Mali-G715 (ARM), OpenGL ES 3.2"))
    }
}
