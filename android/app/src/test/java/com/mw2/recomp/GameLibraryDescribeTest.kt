package com.mw2.recomp

import com.mw2.recomp.GameLibrary.Found
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The game-code line of a bug report (GameLibrary.describeLibrary): what a
 * tester's log must show for the title update's library as for the disc's,
 * and for what each was built, so that a rebuild that was due can be seen.
 */
class GameLibraryDescribeTest {
    private val built = "title awmp17\ngenerated-code ab\n"
    private val newer = "title awmp17\ngenerated-code cd\n"

    @Test
    fun theHashIsTheFirstTwelveDigitsOfTheStatementsSha256() {
        // printf 'title awmp17\ngenerated-code ab\n' | sha256sum, which is how game_abi.cmake names it.
        assertEquals("b573b4e94624", GameLibrary.shortHash(built))
    }

    @Test
    fun aLibraryBuiltHereSaysWhatItWasBuiltFor() {
        val found = Found(builtHere = true, statement = built, expected = built, taken = true, refused = false)
        assertEquals(
            "libawmp17game.so was built on this device for b573b4e94624, which this app takes",
            GameLibrary.describeLibrary("awmp17", found, needed = true),
        )
    }

    @Test
    fun aRebuildThatIsDueNamesBothStatements() {
        val found = Found(builtHere = true, statement = built, expected = newer, taken = false, refused = false)
        assertEquals(
            "libawmp17game.so was built on this device for b573b4e94624, not the c40ceddc3cbe this app takes: a rebuild is due",
            GameLibrary.describeLibrary("awmp17", found, needed = true),
        )
    }

    @Test
    fun aLibraryTheRuntimeRefusedSaysSoAndIsDue() {
        val found = Found(builtHere = true, statement = built, expected = built, taken = true, refused = true)
        assertEquals(
            "libawmp17game.so was built on this device for b573b4e94624, which the runtime refused: a rebuild is due",
            GameLibrary.describeLibrary("awmp17", found, needed = true),
        )
    }

    @Test
    fun aFileWithNoReadableStatementIsDue() {
        val found = Found(builtHere = true, statement = null, expected = newer, taken = false, refused = false)
        assertEquals(
            "libawgame.so was built on this device with no statement this app can read, not the c40ceddc3cbe this app takes: a rebuild is due",
            GameLibrary.describeLibrary("aw", found, needed = true),
        )
    }

    @Test
    fun anAppWithNoBuildKitDoesNotJudge() {
        val found = Found(builtHere = false, statement = built, expected = null, taken = null, refused = false)
        assertEquals(
            "libawgame.so is in the package for b573b4e94624",
            GameLibrary.describeLibrary("aw", found, needed = true),
        )
    }

    @Test
    fun aMissingLibraryIsSaidMissing() {
        assertEquals("libawmp17game.so is MISSING", GameLibrary.describeLibrary("awmp17", null, needed = true))
    }

    @Test
    fun theUpdatesLibraryIsNotNeededBeforeTheUpdateIsAdded() {
        assertEquals(
            "libawmp17game.so not needed (no title update added)",
            GameLibrary.describeLibrary("awmp17", null, needed = false),
        )
    }
}
