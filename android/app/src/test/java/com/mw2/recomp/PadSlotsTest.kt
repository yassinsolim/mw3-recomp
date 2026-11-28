package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PadSlotsTest {

    private fun pad(id: Int, descriptor: String, builtIn: Boolean = false) =
        PadSlots.Device(id, descriptor, descriptor, builtIn)

    @Test
    fun builtInPadIsTheFirstControllerWithTheTouchControls() {
        val slots = PadSlots()
        assertEquals(0 to true, slots.take(pad(5, "thor-pad", builtIn = true)))
        // The Thor shows its pad as two devices; both are the first controller.
        assertEquals(0 to false, slots.take(pad(9, "thor-keys", builtIn = true)))
    }

    @Test
    fun oneBluetoothPadIsTheFirstController() {
        val slots = PadSlots()
        assertEquals(0 to true, slots.take(pad(12, "8bitdo")))
        assertEquals(0, slots.slotOf(12))
    }

    @Test
    fun aBuiltInPadAndOneBluetoothPadAreBothTheFirstController() {
        val slots = PadSlots()
        assertEquals(0 to true, slots.take(pad(5, "thor-pad", builtIn = true)))
        assertEquals(0 to false, slots.take(pad(12, "8bitdo")))
    }

    @Test
    fun bluetoothPadBackUnderANewIdKeepsItsSlot() {
        val slots = PadSlots()
        slots.take(pad(12, "8bitdo"))
        val gone = slots.remove(12)!!
        assertEquals(0, gone.slot)
        assertTrue(gone.slotEmpty)
        assertEquals(0 to true, slots.take(pad(31, "8bitdo")))
    }

    @Test
    fun aRemovalMissedIsFoundWhenTheNewIdPresses() {
        val slots = PadSlots()
        slots.take(pad(12, "8bitdo"))
        // GamepadInput.slotFor drops what is no longer listed before giving a slot.
        val gone = slots.keepOnly(setOf(31))
        assertEquals(listOf(12), gone.map { it.device.id })
        assertEquals(0 to true, slots.take(pad(31, "8bitdo")))
    }

    @Test
    fun theSecondPlayerOfSplitScreenKeepsTheSecondSlotOnReconnecting() {
        val slots = PadSlots()
        assertEquals(0 to true, slots.take(pad(12, "first")))
        assertEquals(1 to true, slots.take(pad(13, "second")))
        slots.remove(13)
        assertEquals(1 to true, slots.take(pad(40, "second")))
        assertEquals(0, slots.slotOf(12))
    }

    @Test
    fun twoPadsAreTwoPlayers() {
        val slots = PadSlots()
        assertEquals(0 to true, slots.take(pad(12, "first")))
        assertEquals(1 to true, slots.take(pad(13, "second")))
        // The first goes: its slot is free for the next pad pressed, the second stays where it is.
        assertTrue(slots.remove(12)!!.slotEmpty)
        assertEquals(1, slots.slotOf(13))
        assertEquals(0 to true, slots.take(pad(14, "third")))
    }

    @Test
    fun twoPadsBesideABuiltInOne() {
        val slots = PadSlots()
        slots.take(pad(5, "thor-pad", builtIn = true))
        assertEquals(0 to false, slots.take(pad(12, "first")))
        assertEquals(1 to true, slots.take(pad(13, "second")))
        // The external pad on slot 0 goes; the built-in one is still there.
        assertFalse(slots.remove(12)!!.slotEmpty)
    }

    @Test
    fun fivePadsLeaveTheFifthWithoutASlot() {
        val slots = PadSlots()
        for (i in 0 until 4) assertEquals(i, slots.take(pad(i, "pad$i"))!!.first)
        assertNull(slots.take(pad(4, "pad4")))
    }

    @Test
    fun aDeviceThatIsNotAPadTakesNoSlot() {
        val keyboard = 0x00000101
        val mouse = 0x00002002
        val stylus = 0x00004002
        val touchscreen = 0x00001002
        assertFalse(PadSlots.isPad(false, keyboard, hasButtonA = false, hasStick = false))
        assertFalse(PadSlots.isPad(false, mouse, hasButtonA = false, hasStick = false))
        assertFalse(PadSlots.isPad(false, stylus or touchscreen, hasButtonA = false, hasStick = false))
        // A keyboard that Android marks as having gamepad keys, with no A button or stick.
        assertFalse(PadSlots.isPad(false, keyboard or PadSlots.SOURCE_GAMEPAD, hasButtonA = false, hasStick = false))
        // The system's virtual devices (adb's `input`) are not a player's pad.
        assertFalse(PadSlots.isPad(true, PadSlots.SOURCE_GAMEPAD, hasButtonA = true, hasStick = false))

        assertTrue(PadSlots.isPad(false, PadSlots.SOURCE_GAMEPAD or PadSlots.SOURCE_JOYSTICK, hasButtonA = true, hasStick = true))
        assertTrue(PadSlots.isPad(false, PadSlots.SOURCE_JOYSTICK, hasButtonA = false, hasStick = true))
    }

    @Test
    fun removingADeviceWithoutASlotChangesNothing() {
        val slots = PadSlots()
        slots.take(pad(12, "first"))
        assertNull(slots.remove(99))
        assertEquals(0, slots.slotOf(12))
    }
}
