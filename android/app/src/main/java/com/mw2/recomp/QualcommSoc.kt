package com.mw2.recomp

import android.os.Build

/**
 * Whether the phone's chip is a Snapdragon: the chips the bundled Turnip
 * drives (DriverStore.installBundledDefault).
 *
 * Android 12 and later say so in Build.SOC_MANUFACTURER ("QTI"). Below it
 * that field does not exist, and every Snapdragon on Android 10 or 11 was
 * taken for something else -- a Realme 5 Pro (Snapdragon 710, Adreno 616,
 * Android 11) was never offered the bundled driver. There the answer is
 * read from what those phones do say: Qualcomm's own board support package
 * names the hardware "qcom", the board is often the platform's name
 * (sdm710, sm6150, msmnile, kona...), and the OpenGL ES driver names an
 * Adreno, which only Qualcomm makes. A MediaTek, Exynos, Tensor, Unisoc or
 * Kirin phone says none of these: "mt6893", "exynos9820", "Mali-G77".
 */
object QualcommSoc {

    /** The phone this runs on. [gpu] is DeviceInfo.gpu's text, asked only below Android 12. */
    fun detect(gpu: () -> String): Boolean {
        val manufacturer = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MANUFACTURER else null
        return isQualcomm(Build.VERSION.SDK_INT, manufacturer, Build.HARDWARE, Build.BOARD, gpu)
    }

    /**
     * [socManufacturer]: Build.SOC_MANUFACTURER, null below API 31.
     * [hardware], [board]: Build.HARDWARE (ro.hardware) and Build.BOARD (ro.product.board).
     * [gpu]: "<GL renderer> (<GL vendor>), <version>; ..." as DeviceInfo.gpu writes it.
     */
    fun isQualcomm(sdk: Int, socManufacturer: String?, hardware: String, board: String, gpu: () -> String): Boolean {
        // Where the system says, it is believed: this is the rule Android 12
        // and later phones (the AYN Thor, the Retroid Pocket Flip 2) have
        // always been given.
        if (sdk >= 31) return socManufacturer == "QTI"
        if (hardware.lowercase() == "qcom") return true
        if (qualcommPlatform(board)) return true
        val text = try {
            gpu()
        } catch (_: Throwable) {
            ""
        }
        return text.contains("Adreno", ignoreCase = true) || text.contains("(Qualcomm)")
    }

    /** sdm710, sm6150, msm8953, apq8096, sda845, qcs605 and the platforms Qualcomm named instead of numbering. */
    private fun qualcommPlatform(board: String): Boolean {
        val name = board.lowercase()
        return numbered.matches(name) || name in codenames
    }

    private val numbered = Regex("(msm|sdm|sm|apq|sda|qcs|qcm)\\d{3,4}[a-z]*")

    // Qualcomm's codenames for the platforms phones of Android 10 and 11 were
    // built on (ro.board.platform, and ro.product.board on Samsung's and
    // others' Snapdragon models). None is a MediaTek, Exynos or Unisoc name.
    private val codenames = setOf(
        "msmnile",   // Snapdragon 855/860
        "kona",      // 865/870
        "lahaina",   // 888
        "lito",      // 765/750G
        "atoll",     // 720G
        "sdmmagpie", // 730
        "trinket",   // 665
        "bengal",    // 662/460
        "holi",      // 480
        "taro",      // 8 Gen 1
    )
}
