package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The presets' table (GraphicsPresets.kt): what each sets, which preset an
 * older install's settings are, and when Best looking draws at twice the
 * size.
 */
class GraphicsPresetsTest {

    @Test
    fun automaticIsWhatAFreshInstallHas() {
        val values = GraphicsPresets.values(GraphicsPresets.AUTOMATIC)!!
        assertEquals(70, values.resolution)
        assertEquals("the runtime's limit", 0, values.fpsLimit)
        assertEquals(GraphicsPresets.SHADOWS_AUTO, values.sunShadows)
        assertEquals(1, values.renderScale)
        assertEquals(GraphicsPresets.SCENE_FOLLOWS, values.scene)
    }

    @Test
    fun eachPresetIsWhatItsLineSays() {
        val battery = GraphicsPresets.values(GraphicsPresets.BATTERY)!!
        assertEquals(30, battery.fpsLimit)
        assertEquals(GraphicsPresets.SCENE_SMALLER, battery.scene)
        assertEquals(GraphicsPresets.SHADOWS_OFF, battery.sunShadows)

        val balanced = GraphicsPresets.values(GraphicsPresets.BALANCED)!!
        assertEquals(60, balanced.fpsLimit)
        assertEquals(GraphicsPresets.SCENE_FOLLOWS, balanced.scene)
        assertEquals(GraphicsPresets.SHADOWS_AUTO, balanced.sunShadows)

        val best = GraphicsPresets.values(GraphicsPresets.BEST)!!
        assertEquals("the screen's rate, where the device keeps it", 0, best.fpsLimit)
        assertEquals(GraphicsPresets.SCENE_WHOLE, best.scene)
        assertEquals(GraphicsPresets.SHADOWS_ON, best.sunShadows)
        assertEquals(100, best.resolution)
        assertEquals("1x until it was measured", 1, best.renderScale)
        assertEquals(2, GraphicsPresets.values(GraphicsPresets.BEST, twice = true)!!.renderScale)

        assertNull("Custom sets nothing", GraphicsPresets.values(GraphicsPresets.CUSTOM))
    }

    @Test
    fun onlyBestLookingDrawsAtTwiceTheSize() {
        for (preset in listOf(GraphicsPresets.AUTOMATIC, GraphicsPresets.BATTERY, GraphicsPresets.BALANCED)) {
            assertEquals(1, GraphicsPresets.values(preset, twice = true)!!.renderScale)
        }
    }

    @Test
    fun anOlderInstallIsThePresetItsSettingsAre() {
        assertEquals(
            GraphicsPresets.AUTOMATIC,
            GraphicsPresets.matching(GraphicsPresets.Values(70, 0, 0, 1, GraphicsPresets.SCENE_FOLLOWS)),
        )
        assertEquals(
            "a resolution somebody chose",
            GraphicsPresets.CUSTOM,
            GraphicsPresets.matching(GraphicsPresets.Values(85, 0, 0, 1, GraphicsPresets.SCENE_FOLLOWS)),
        )
        assertEquals(
            "a limit of 45, which the older versions offered",
            GraphicsPresets.CUSTOM,
            GraphicsPresets.matching(GraphicsPresets.Values(70, 45, 0, 1, GraphicsPresets.SCENE_FOLLOWS)),
        )
        for (preset in GraphicsPresets.AUTOMATIC..GraphicsPresets.BEST) {
            assertEquals(preset, GraphicsPresets.matching(GraphicsPresets.values(preset)!!))
        }
    }

    @Test
    fun twiceTheSizeNeedsAMeasureWithRoom() {
        assertFalse("nothing measured", GraphicsPresets.twiceHolds(-1f, 120))
        // Advanced Warfare on the AYN Thor: 3.6 ms at 1x was 7.6 at 2x, of the 8.3 a frame has at 120.
        assertFalse(GraphicsPresets.twiceHolds(3.6f, 120))
        assertTrue("the same chip time at 60", GraphicsPresets.twiceHolds(3.6f, 60))
        assertTrue(GraphicsPresets.twiceHolds(3.0f, 120))
        assertFalse(GraphicsPresets.twiceHolds(7.0f, 60))
        assertFalse(GraphicsPresets.twiceHolds(1f, 0))
    }

    @Test
    fun twiceTheSizeIsGivenUpWhenItDidNotHold() {
        assertFalse(GraphicsPresets.twiceFailed(0.02f, 120, 120))
        assertTrue("a tenth of the play came late", GraphicsPresets.twiceFailed(0.2f, 120, 120))
        assertTrue("held to a slower rate", GraphicsPresets.twiceFailed(0f, 60, 120))
        assertFalse("no rate read yet", GraphicsPresets.twiceFailed(0f, 0, 120))
    }

    @Test
    fun thePresetsHaveNames() {
        assertEquals("automatic", GraphicsPresets.name(GraphicsPresets.AUTOMATIC))
        assertEquals("battery saver", GraphicsPresets.name(GraphicsPresets.BATTERY))
        assertEquals("balanced", GraphicsPresets.name(GraphicsPresets.BALANCED))
        assertEquals("best looking", GraphicsPresets.name(GraphicsPresets.BEST))
        assertEquals("custom", GraphicsPresets.name(GraphicsPresets.CUSTOM))
    }
}
