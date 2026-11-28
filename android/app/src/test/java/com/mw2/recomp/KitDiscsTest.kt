package com.mw2.recomp
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/**
 * The executables each game's setup accepts from a disc: the kit the build
 * makes (build.gradle.kts, kitGames) is what the app checks a disc image
 * against, and the installer built into its runtime checks the files it
 * installs against CMakeLists.txt's hashes (tests/xex_hashes_test.cmake holds
 * those two together). What the one accepts the other installs, so a disc is
 * not passed here and refused there with a sentence of the installer's.
 */
class KitDiscsTest {
    @get:Rule
    val folder = TemporaryFolder()

    /** What the build wrote (the unit tests run after the kit tasks: build.gradle.kts). */
    private fun kit(game: String): GameBuild.Kit =
        GameBuild.Kit(JSONObject(File(System.getProperty("buildKitDirectory")!!, "$game/buildkit/kit.json").readText()))

    /** The supported builds of each executable the player's disc gives, by the executable's name. */
    private fun accepted(game: String): Map<String, List<String>> =
        GameBuild.fromDisc(kit(game)).associate { it.executable to it.sha256 }

    private val mw2 = mapOf(
        "default.xex" to listOf("990e640d3a92544f9d585e71faf1ab65082d37ec6f5865b2599335fa123bf6ff"),
        "default_mp.xex" to listOf("aacc47aff3221f3bd9129e80827fd3bcd74aa43bf74bb5c5ddbd0597c0d5fcde"),
    )
    private val cod4 = mapOf(
        "default.xex" to listOf("f6b848ac4ecf2b12881223e4c3cd63c49ae36e1fadc6f2179428033274602739"),
        "default_mp.xex" to listOf("9aae8db5b38f0b394f9052d1ee9c8b1f18038ff86fc5f4110f173f66043a0cc6"),
    )
    private val aw = mapOf(
        "default.xex" to listOf("a8b1e99c27d6d03a0b0700e6eada1e7d24ee4b194e3c9fa844f16c25f8376a73"),
        "default_mp.xex" to listOf("3ef96359f96ee7994e43de70f56a9c2a019d5b841f11318ca2dffdb717b265f8"),
    )
    // The multiplayer and the zombies: the disc's, of which the title update's patch makes what runs.
    private val bo3 = mapOf(
        "default.xex" to listOf("e64ed5fa25ca8c97d3970efc9b0d17bc32253cc43dbf9f5f96637aada064d8bb"),
        "default_zm.xex" to listOf("d0eb63c97f69cf14ecaf5171429fa4afcce6f0fd5ca3d774c7a0af343f33fe4e"),
    )

    // What the published Modern Warfare 3 app installs (android-release.yml's MW3_XEX_SHA256, CMakeLists.txt's
    // default): the USA/Europe disc's campaign executable, version 2, and the multiplayer's.
    private val mw3Release = "e94de720373d668a2bd592dfc74339bfbbc98cca993176fcd055b1449633bcd1"
    private val mw3BringUp = "d90bc8d93b7b8f6001ead8330271196a6ef7a5c2e4a9e9b63118f47d58a05912"
    private val mw3Multiplayer = "0d7669844122b8f94e29ec87baf6bf0a7b84ab4abad8a82b43aa444682f90f7c"

    /** True when this machine's environment or tree changes what an MW3 build installs (build.gradle.kts, mw3Executable). */
    private fun mw3IsChosenHere() = !System.getenv("MW3_XEX_SHA256").isNullOrBlank() ||
        !System.getenv("MW3_XEX_SHA256_MP").isNullOrBlank() ||
        File("../../mw3/tu0/default.xex").isFile || File("../../mw3/tu0/default_mp.xex").isFile

    @Test
    fun modernWarfare2TakesItsOneDisc() = assertEquals(mw2, accepted("mw2"))

    @Test
    fun callOfDuty4TakesItsOneDisc() = assertEquals(cod4, accepted("cod4"))

    @Test
    fun advancedWarfareTakesItsDiscAndNothingForTheTitleUpdatesExecutable() {
        assertEquals(aw, accepted("aw"))
        // The multiplayer the map packs make is made on the device, from the multiplayer above and the update.
        assertTrue(kit("aw").titles.single { it.library == "awmp17" }.fromUpdate)
    }

    @Test
    fun blackOpsIIITakesTheDiscsTwoExecutables() = assertEquals(bo3, accepted("bo3"))

    @Test
    fun modernWarfare3TakesTheDiscThePublishedAppInstallsAndNoOther() {
        assumeTrue(!mw3IsChosenHere())
        val taken = accepted("mw3")
        assertEquals(mapOf("default.xex" to listOf(mw3Release), "default_mp.xex" to listOf(mw3Multiplayer)), taken)
        // The bring-up's packaging (version 22) is what the installer of a release refuses, so this refuses it too.
        assertFalse(mw3BringUp in taken.getValue("default.xex"))
        assertEquals(listOf(2L), kit("mw3").titles.single { it.library == "mw3" }.versions)
    }

    @Test
    fun modernWarfare3TakesWhatTheBuildInstallsWhereTheBuildMachineChoosesIt() {
        // The build machine's MW3_XEX_SHA256, else the executable in mw3/tu0 as CMake hashes it, else the release's.
        val chosen = System.getenv("MW3_XEX_SHA256")?.trim()?.ifEmpty { null }
            ?: File("../../mw3/tu0/default.xex").takeIf { it.isFile }?.let { sha256(it.readBytes()) }
            ?: mw3Release
        assertEquals(listOf(chosen), accepted("mw3").getValue("default.xex"))
    }

    // ---- what a player whose disc has the other packaging is told ------------------------------

    /** An executable's header as far as its version (DiscRefusalTest has the same). */
    private fun xex(version: Int, padding: Int = 0): ByteArray {
        val data = ByteBuffer.allocate(0x60 + padding).order(ByteOrder.BIG_ENDIAN)
        data.putInt(0, 0x58455832) // "XEX2"
        data.putInt(20, 1) // one optional header
        data.putInt(24, 0x00040006) // execution info
        data.putInt(28, 0x40) // where it is
        data.putInt(0x44, version)
        data.putInt(0x48, version)
        data.putInt(0x4C, 0x415608CB) // Modern Warfare 3
        return data.array()
    }

    private fun sha256(data: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }

    @Test
    fun theBringUpsPackagingIsRefusedWithItsVersionAndTheOneSupported() {
        assumeTrue(!mw3IsChosenHere())
        val image = folder.newFile().also { it.writeBytes(xex(22)) }
        val title = GameBuild.fromDisc(kit("mw3")).single { it.executable == "default.xex" }
        val refused = try {
            RandomAccessFile(image, "r").channel.use { GameBuild.take(File(folder.root, "kept/default.xex"), title, it, 0, image.length()) }
            null
        } catch (refusal: DiscRefusal) {
            refusal
        }
        assertTrue(refused is DiscRefusal.UnsupportedVersion)
        refused as DiscRefusal.UnsupportedVersion
        assertEquals(22L, refused.found)
        assertEquals(listOf(2L), refused.supported)
        assertFalse(File(folder.root, "kept/default.xex").exists())
    }
}
