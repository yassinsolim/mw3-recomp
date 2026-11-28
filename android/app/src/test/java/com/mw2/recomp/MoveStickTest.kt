package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The movement stick's rules without a phone: where a push is measured from
 * (the stick's own centre, or where a floating stick's finger came down),
 * which side a floating stick takes touches on, and when auto-sprint presses
 * and lets go.
 */
class MoveStickTest {
    private val out = FloatArray(2)

    @Test
    fun aPushIsMeasuredFromTheOriginWithYPointingUp() {
        // A floating stick put down at (300, 800): a finger 30 px to the right and 60 px above it, radius 120.
        MoveStick.deflection(330f, 740f, 300f, 800f, 120f, out)
        assertEquals(0.25f, out[0], 1e-6f)
        assertEquals(0.5f, out[1], 1e-6f)
    }

    @Test
    fun theFingerWhereItCameDownIsNotAPush() {
        MoveStick.deflection(412f, 633f, 412f, 633f, 100f, out)
        assertEquals(0f, out[0], 0f)
        assertEquals(0f, out[1], 0f)
    }

    @Test
    fun noPushIsPastTheEdgeWhateverTheDistanceAndTheDirectionIsKept() {
        MoveStick.deflection(300f + 300f, 800f + 400f, 300f, 800f, 100f, out)   // 500 px away, 3-4-5
        assertEquals(0.6f, out[0], 1e-6f)
        assertEquals(-0.8f, out[1], 1e-6f)   // below the origin on the screen is down for the guest
        assertEquals(1f, MoveStick.magnitude(out[0], out[1]), 1e-6f)
    }

    @Test
    fun aStickFollowsItsOriginNotItsPlace() {
        // The same finger spot is a push on one origin and none on another.
        MoveStick.deflection(500f, 500f, 500f, 500f, 100f, out)
        assertEquals(0f, MoveStick.magnitude(out[0], out[1]), 0f)
        MoveStick.deflection(500f, 500f, 400f, 500f, 100f, out)
        assertEquals(1f, MoveStick.magnitude(out[0], out[1]), 1e-6f)
    }

    @Test
    fun aFloatingStickTakesTheSideItRestsIn() {
        // Resting on the left (default layout): the left half takes it, the right half is the look area.
        assertTrue(MoveStick.onMovementSide(10f, 1920f, 0.13f))
        assertTrue(MoveStick.onMovementSide(959f, 1920f, 0.13f))
        assertFalse(MoveStick.onMovementSide(960f, 1920f, 0.13f))
        assertFalse(MoveStick.onMovementSide(1800f, 1920f, 0.13f))
        // A left-handed layout with the stick moved to the right.
        assertTrue(MoveStick.onMovementSide(960f, 1920f, 0.8f))
        assertFalse(MoveStick.onMovementSide(100f, 1920f, 0.8f))
    }

    @Test
    fun sprintIsPressedAfterTheDelayAtTheEdgeAndNotBefore() {
        val s = AutoSprint(300)
        assertFalse(s.update(1f, 1000))
        assertEquals(1300L, s.nextChangeAt(1000))
        assertFalse(s.update(1f, 1299))
        assertTrue(s.update(1f, 1300))
        assertTrue(s.sprinting)
        assertEquals(AutoSprint.NOT_YET, s.nextChangeAt(1300))
    }

    @Test
    fun aThumbHeldStillAtTheEdgeSprintsWhenTheViewIsToldToLook() {
        // No touch events come in between: the state is asked again at nextChangeAt.
        val s = AutoSprint(300)
        s.update(0.95f, 5000)
        val due = s.nextChangeAt(5100)
        assertEquals(5300L, due)
        assertTrue(s.update(0.95f, due))
    }

    @Test
    fun anEdgeShorterThanTheDelayDoesNotSprintAndStartsOverNextTime() {
        val s = AutoSprint(300)
        s.update(1f, 1000)
        assertFalse(s.update(0.5f, 1200))     // pulled back
        assertEquals(AutoSprint.NOT_YET, s.nextChangeAt(1200))
        assertFalse(s.update(1f, 1250))       // back to the edge: the count starts here
        assertFalse(s.update(1f, 1500))
        assertTrue(s.update(1f, 1550))
    }

    @Test
    fun sprintIsLetGoWhenThePushFallsBelowTheWayBack() {
        val s = AutoSprint(300)
        s.update(1f, 0)
        assertTrue(s.update(1f, 300))
        assertTrue(s.update(0.8f, 400))       // between the two: stays
        assertTrue(s.update(AutoSprint.RELEASE_AT, 450))
        assertFalse(s.update(0.69f, 500))
        // And stays let go until the edge is held again.
        assertFalse(s.update(0.8f, 600))
        assertFalse(s.update(0.95f, 700))
        assertTrue(s.update(0.95f, 1000))
    }

    @Test
    fun theFingerComingUpLetsGoAtOnce() {
        val s = AutoSprint(300)
        s.update(1f, 0)
        s.update(1f, 300)
        assertTrue(s.sprinting)
        assertFalse(s.update(0f, 320))
    }

    @Test
    fun aWobbleAtTheEdgeNeitherStartsNorStopsItByTurns() {
        val s = AutoSprint(300)
        // Between 0.9 and 1.0 all the time: counts as the edge throughout.
        for (t in 0L..250L step 50L) assertFalse(s.update(if (t % 100L == 0L) 0.92f else 1f, t))
        assertTrue(s.update(0.91f, 300))
        // Dipping to 0.75 afterwards, above the way back, does not let go.
        assertTrue(s.update(0.75f, 350))
        assertTrue(s.update(0.95f, 400))
    }

    @Test
    fun cancelLetsGoAndForgetsTheCount() {
        val s = AutoSprint(300)
        s.update(1f, 0)
        s.cancel()
        assertEquals(AutoSprint.NOT_YET, s.nextChangeAt(10))
        assertFalse(s.update(1f, 400))        // 400 ms after the first touch but the count began again at 400
        assertTrue(s.update(1f, 700))
    }

    @Test
    fun theDefaultsAreAThirdOfASecondAndTheEdgeIsAboveTheWayBack() {
        assertEquals(300L, AutoSprint().delayMillis)
        assertTrue(AutoSprint.PUSH_AT > AutoSprint.RELEASE_AT)
        assertTrue(AutoSprint.PUSH_AT <= 1f)
    }
}
