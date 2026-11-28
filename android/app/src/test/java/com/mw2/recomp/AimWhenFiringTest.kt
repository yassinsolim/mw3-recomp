package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * "Aim when firing (touch)" as timing: Fire down holds the left trigger at
 * once and the right one after the delay; Fire up lets go of both; and a
 * press with Aim already held is left alone.
 */
class AimWhenFiringTest {
    private val delay = 150L
    private val shot = AimWhenFiring.MIN_SHOT_MILLIS

    private fun aimFire() = AimWhenFiring(delay)

    @Test
    fun theSightsComeUpAtOnceAndTheShotFollowsTheDelay() {
        val a = aimFire()
        assertTrue(a.press(1000, aimingAlready = false))
        assertTrue(a.managing(1000))
        assertTrue(a.leftTrigger(1000))
        assertFalse(a.rightTrigger(1000))
        assertTrue(a.leftTrigger(1149))
        assertFalse(a.rightTrigger(1149))
        assertTrue(a.leftTrigger(1150))
        assertTrue(a.rightTrigger(1150))
        // Held on: both stay down for as long as Fire is.
        assertTrue(a.leftTrigger(60_000))
        assertTrue(a.rightTrigger(60_000))
        assertEquals(AimWhenFiring.NOT_YET, a.nextChangeAt(60_000))
    }

    @Test
    fun theViewIsToldWhenTheRightTriggerComes() {
        val a = aimFire()
        a.press(1000, false)
        assertEquals(1150L, a.nextChangeAt(1000))
        assertEquals(1150L, a.nextChangeAt(1149))
        assertEquals(AimWhenFiring.NOT_YET, a.nextChangeAt(1150))
    }

    @Test
    fun releasingFireReleasesBothAtOnceOnceTheShotHasHadItsMinimum() {
        val a = aimFire()
        a.press(1000, false)
        a.release(2000)
        assertFalse(a.managing(2000))
        assertFalse(a.leftTrigger(2000))
        assertFalse(a.rightTrigger(2000))
        assertEquals(AimWhenFiring.NOT_YET, a.nextChangeAt(2000))
    }

    @Test
    fun aTapShorterThanTheDelayStillShootsAndLetsGoOfBothTogether() {
        val a = aimFire()
        a.press(1000, false)
        a.release(1040)                      // 40 ms: the sights are not up yet
        assertTrue(a.leftTrigger(1100))
        assertFalse(a.rightTrigger(1100))    // not before the delay
        assertTrue(a.rightTrigger(1150))     // the shot, held for its minimum
        assertTrue(a.rightTrigger(1150 + shot - 1))
        assertEquals(1150 + shot, a.nextChangeAt(1150))
        // Both up on the same instant.
        assertFalse(a.leftTrigger(1150 + shot))
        assertFalse(a.rightTrigger(1150 + shot))
    }

    @Test
    fun aPressWithAimAlreadyHeldIsLeftAloneAndHoldsNothing() {
        val a = aimFire()
        assertFalse(a.press(1000, aimingAlready = true))
        assertFalse(a.managing(1000))
        assertFalse(a.leftTrigger(1000))
        assertFalse(a.rightTrigger(5000))
        assertEquals(AimWhenFiring.NOT_YET, a.nextChangeAt(1000))
        a.release(1500)   // releasing what was never managed changes nothing
        assertFalse(a.managing(1500))
    }

    @Test
    fun firingAgainDuringATapsShotKeepsTheSightsAndFiresAtOnce() {
        val a = aimFire()
        a.press(1000, false)
        a.release(1010)
        // Shot runs 1150..1220; the finger is back at 1180, the sights up since 1000.
        assertTrue(a.press(1180, aimingAlready = false))
        assertTrue(a.leftTrigger(1180))
        assertTrue(a.rightTrigger(1180))
        assertTrue(a.rightTrigger(5000))
        a.release(5000)
        assertFalse(a.managing(5000 + shot))
    }

    @Test
    fun aSecondPressAfterTheFirstEndedStartsOverWithTheDelay() {
        val a = aimFire()
        a.press(1000, false)
        a.release(1010)
        assertFalse(a.managing(1150 + shot))
        assertTrue(a.press(2000, false))
        assertTrue(a.leftTrigger(2000))
        assertFalse(a.rightTrigger(2100))
        assertTrue(a.rightTrigger(2150))
    }

    @Test
    fun aSecondPressWhileFireIsStillDownJustPressesAgain() {
        val a = aimFire()
        a.press(1000, false)
        a.press(1100, false)   // a second finger on Fire, or a toggle pressed twice
        assertEquals(1150L, a.nextChangeAt(1100))
        assertTrue(a.rightTrigger(1150))
    }

    @Test
    fun cancelLetsGoOfEverything() {
        val a = aimFire()
        a.press(1000, false)
        a.cancel()
        assertFalse(a.managing(1200))
        assertFalse(a.leftTrigger(1200))
        assertFalse(a.rightTrigger(1200))
        // And the next press starts clean.
        assertTrue(a.press(3000, false))
        assertFalse(a.rightTrigger(3100))
    }

    @Test
    fun aZeroDelayFiresWithTheSights() {
        val a = AimWhenFiring(0)
        a.press(1000, false)
        assertTrue(a.leftTrigger(1000))
        assertTrue(a.rightTrigger(1000))
    }

    @Test
    fun theDefaultDelayIsAShortOne() {
        assertTrue(AimWhenFiring.DEFAULT_DELAY_MILLIS in 100L..250L)
        assertEquals(AimWhenFiring.DEFAULT_DELAY_MILLIS, AimWhenFiring().delayMillis)
    }
}
