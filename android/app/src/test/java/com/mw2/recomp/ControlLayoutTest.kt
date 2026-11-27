package com.mw2.recomp

import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The layout a new player starts with, on the screens phones have: the look
 * area is the right half, and what stands in it leaves most of it free,
 * stays on the glass and does not lie over its neighbour. Positions are
 * shares of the screen and sizes are in dp, so a narrow screen is where
 * controls meet; 360 dp is as low as a phone's landscape gets.
 */
class ControlLayoutTest {

    private class Screen(val name: String, val width: Float, val height: Float)

    private val screens = listOf(
        Screen("16:9, 640 by 360 dp", 640f, 360f),
        Screen("20:9, 800 by 360 dp", 800f, 360f),
        Screen("16:9, 731 by 411 dp", 731f, 411f),
    )

    private fun lookSide(layout: ControlLayout): List<ControlElement> =
        layout.elements.filter { it.kind != ControlKind.LOOK && it.visible && it.xFrac >= 0.5f }

    @Test
    fun theLookAreaIsTheRightHalfWithNoStickInIt() {
        val layout = ControlLayout.defaults()
        val look = layout.elements.single { it.kind == ControlKind.LOOK }
        assertTrue(look.visible && look.xFrac >= 0.5f)
        assertEquals(false, layout.find("right_stick")!!.visible)
        assertTrue(lookSide(layout).none { it.kind == ControlKind.STICK })
    }

    @Test
    fun theButtonsBesideTheLookAreaStayOnTheGlassAndApart() {
        for (screen in screens) {
            val controls = lookSide(ControlLayout.defaults())
            for (control in controls) {
                val x = control.xFrac * screen.width
                val y = control.yFrac * screen.height
                val radius = control.sizeDp / 2f
                assertTrue(
                    "${control.id} runs off a screen of ${screen.name}",
                    x - radius >= 0f && x + radius <= screen.width && y - radius >= 0f && y + radius <= screen.height,
                )
            }
            for (i in controls.indices) {
                for (j in i + 1 until controls.size) {
                    val a = controls[i]
                    val b = controls[j]
                    val apart = hypot((a.xFrac - b.xFrac) * screen.width, (a.yFrac - b.yFrac) * screen.height)
                    assertTrue(
                        "${a.id} lies over ${b.id} on a screen of ${screen.name}",
                        apart >= (a.sizeDp + b.sizeDp) / 2f,
                    )
                }
            }
        }
    }

    @Test
    fun mostOfTheLookAreaIsFreeOfButtons() {
        for (screen in screens) {
            val controls = lookSide(ControlLayout.defaults())
            // A touch a little outside a button still presses it
            // (TouchOverlayView.hit): 6 dp.
            var taken = 0
            val steps = 200
            for (i in 0 until steps) {
                for (j in 0 until steps) {
                    val x = screen.width * (0.5f + (i + 0.5f) / steps / 2f)
                    val y = screen.height * (j + 0.5f) / steps
                    if (controls.any { hypot(x - it.xFrac * screen.width, y - it.yFrac * screen.height) <= it.sizeDp / 2f + 6f }) {
                        taken++
                    }
                }
            }
            val free = 1f - taken.toFloat() / (steps * steps)
            assertTrue("only ${(free * 100).toInt()}% of the look area is free on ${screen.name}", free >= 0.7f)
        }
    }

    @Test
    fun onlyTheLookAreaHasNoSizeOrOpacityToEdit() {
        for (element in ControlLayout.defaults().elements) {
            assertEquals(element.id, element.kind != ControlKind.LOOK, element.adjustable)
        }
    }
}
