package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The card over the black screen while Compile shaders at boot runs
 * (BootCompile), from the values NativeBridge.nativeStats fills, with the
 * numbers of mw2 #4: a Galaxy A35 (Mali-G68, r38p1) compiling 1,064 recorded
 * pipelines before the first frame. No phone is involved.
 */
class BootCompileTest {

    private fun stats(total: Int, done: Int, kept: Boolean) = FloatArray(BootCompile.STATS_VALUES).also {
        it[19] = total.toFloat()
        it[20] = done.toFloat()
        it[21] = if (kept) 1f else 0f
    }

    @Test
    fun aRuntimeThatDoesNotReportItShowsNothing() {
        // The array the app filled before these values existed.
        assertNull(BootCompile.from(FloatArray(19)))
    }

    @Test
    fun noBootCompileIsNotRunning() {
        // Compile shaders at boot off: the runtime reports no total.
        val boot = BootCompile.from(stats(0, 0, kept = true))!!
        assertFalse(boot.running)
        assertEquals(0, boot.percent)
    }

    @Test
    fun theA35sCompileAsItGoes() {
        val start = BootCompile.from(stats(1064, 0, kept = true))!!
        assertTrue(start.running)
        assertEquals(0, start.percent)

        // Where its log stopped: "compiling shaders: 40%".
        val partWay = BootCompile.from(stats(1064, 426, kept = true))!!
        assertTrue(partWay.running)
        assertEquals(40, partWay.percent)

        val last = BootCompile.from(stats(1064, 1063, kept = true))!!
        assertTrue(last.running)
        assertEquals(99, last.percent)

        // All gone through: the card goes, even before the first frame.
        val done = BootCompile.from(stats(1064, 1064, kept = true))!!
        assertFalse(done.running)
        assertEquals(100, done.percent)
    }

    @Test
    fun aCountPastTheTotalIsHeldToIt() {
        assertEquals(100, BootCompile(10, 12, kept = true).percent)
        assertEquals(0, BootCompile(10, -1, kept = true).percent)
    }

    @Test
    fun theSentenceSaysWhetherTheWaitComesBack() {
        // r38 on (pipeline_cache_rules.h): kept, a one-time wait.
        assertEquals(R.string.boot_compile_once, BootCompile.from(stats(1064, 10, kept = true))!!.note)
        // A Mali driver older than r38: every start compiles them again.
        assertEquals(R.string.boot_compile_every_start, BootCompile.from(stats(1064, 10, kept = false))!!.note)
    }
}
