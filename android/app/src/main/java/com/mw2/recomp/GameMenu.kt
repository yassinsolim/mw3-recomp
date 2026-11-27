package com.mw2.recomp

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * The menu over a running game: what the launcher's Settings offer, changed
 * without leaving the level, and the run's own entries (the pad editor, the
 * bots, quitting).
 *
 * It is a panel of the app's own, the same in every title, rather than rows
 * in each title's pause menu: those are four menu systems in fastfiles the
 * app does not ship. It sits at the left of the picture and leaves the rest
 * clear, and the game goes on under it, so a setting is seen taking effect
 * as it is changed.
 *
 * It has to work with nothing but a pad, since the on-screen controls are
 * one of the things it turns off: the D-pad or a stick chooses a row, left
 * and right change its value, A takes it and B goes back. A finger does the
 * same by touching a row or its arrows. GameActivity opens it and hands it
 * the pad's events while it is up.
 */
class GameMenu(context: Context, private val host: FrameLayout, private val onClosed: () -> Unit) {

    /** A line of a page. */
    sealed interface Row {
        val label: String
    }

    /** A line that does something when taken: opens a page, or closes the menu and acts. */
    class Entry(override val label: String, val run: () -> Unit) : Row

    /**
     * A setting with a few values in a row. [set] is given the new value's
     * place the moment it is chosen and applies it; [afterRestart] marks one
     * that the running game cannot take.
     */
    class Choice(
        override val label: String,
        val values: List<String>,
        var index: Int,
        val afterRestart: Boolean = false,
        val set: (Int) -> Unit,
    ) : Row

    /** A list of lines under a heading, with a sentence below them when there is one to say. */
    class Page(val title: String, val rows: List<Row>, val note: String = "")

    private val density = context.resources.displayMetrics.density
    private fun dp(value: Int): Int = (value * density).roundToInt()

    private val accent = Color.rgb(255, 122, 26)
    private val ink = Color.rgb(236, 238, 242)
    private val faint = Color.rgb(160, 168, 180)

    // The pages under the one shown, for B and the back key.
    private val under = ArrayList<Pair<Page, Int>>()
    private var page: Page? = null
    private var selected = 0
    private val rowViews = ArrayList<LinearLayout>()
    private val valueViews = HashMap<Choice, TextView>()

    // The whole screen while the menu is up: a touch beside the panel closes
    // it, and none reaches the on-screen pad underneath.
    private val scrim = FrameLayout(context).apply {
        setOnClickListener { close() }
        isSoundEffectsEnabled = false
    }
    private val heading = TextView(context).apply {
        setTextColor(accent)
        textSize = 19f
        typeface = Typeface.DEFAULT_BOLD
        setPadding(dp(14), dp(2), dp(14), dp(8))
    }
    private val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val scroll = ScrollView(context).apply {
        isVerticalScrollBarEnabled = false
        addView(list)
    }
    private val note = TextView(context).apply {
        setTextColor(faint)
        textSize = 12.5f
        setPadding(dp(14), dp(8), dp(14), 0)
    }
    private val panel = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        background = GradientDrawable().apply {
            setColor(Color.argb(232, 20, 20, 23))
            cornerRadius = dp(14).toFloat()
            setStroke(dp(1), Color.argb(200, 255, 122, 26))
        }
        setPadding(dp(6), dp(10), dp(6), dp(10))
        // A touch on the panel itself is not one beside it.
        isClickable = true
        addView(heading)
        addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        addView(note)
    }

    // Which way the D-pad's hat or the left stick last pointed, so that a
    // push is one step and not one for every event it sends while held.
    private var pushedX = 0
    private var pushedY = 0

    val isOpen: Boolean
        get() = scrim.parent != null

    fun open(first: Page) {
        under.clear()
        if (!isOpen) {
            // Half the screen at most, so the picture beside it shows what a
            // setting did; never wider than its lines need.
            val width = min(dp(460), host.width / 2).coerceAtLeast(dp(300))
            scrim.removeAllViews()
            scrim.addView(
                panel,
                // Below the frame-rate line at the top, which a cap or a
                // resolution chosen here is read from.
                FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.START or Gravity.TOP)
                    .apply { marginStart = dp(18); topMargin = dp(40); bottomMargin = dp(10) },
            )
            host.addView(scrim, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        show(first, 0)
    }

    /** A page over the one shown; B comes back from it. */
    fun push(next: Page) {
        page?.let { under.add(it to selected) }
        show(next, 0)
    }

    /** The page shown, made again: a row's choice changed what the others say. The row chosen stays chosen. */
    fun replace(next: Page) {
        if (isOpen) show(next, selected)
    }

    fun close() {
        if (!isOpen) return
        host.removeView(scrim)
        page = null
        under.clear()
        onClosed()
    }

    /** B, the back key or the back gesture: the page under this one, or out. */
    fun back() {
        val last = under.removeLastOrNull()
        if (last == null) close() else show(last.first, last.second)
    }

    @SuppressLint("SetTextI18n")
    private fun show(next: Page, select: Int) {
        page = next
        heading.text = next.title
        note.text = next.note
        note.visibility = if (next.note.isEmpty()) View.GONE else View.VISIBLE
        list.removeAllViews()
        rowViews.clear()
        valueViews.clear()
        val context = host.context
        next.rows.forEachIndexed { at, row ->
            val line = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                minimumHeight = dp(42)
                setPadding(dp(12), dp(4), dp(6), dp(4))
                isSoundEffectsEnabled = false
                setOnClickListener {
                    select(at)
                    if (row is Entry) row.run()
                }
            }
            line.addView(
                TextView(context).apply {
                    text = if (row is Choice && row.afterRestart) row.label + " *" else row.label
                    setTextColor(ink)
                    textSize = 16f
                },
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f),
            )
            if (row is Choice) {
                line.addView(arrow("◀") { select(at); step(row, -1) })
                val value = TextView(context).apply {
                    setTextColor(accent)
                    textSize = 16f
                    typeface = Typeface.DEFAULT_BOLD
                    gravity = Gravity.CENTER
                    maxLines = 2
                }
                valueViews[row] = value
                line.addView(value, LinearLayout.LayoutParams(dp(128), ViewGroup.LayoutParams.WRAP_CONTENT))
                line.addView(arrow("▶") { select(at); step(row, 1) })
                paint(row)
            }
            rowViews.add(line)
            list.addView(line, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        select(select)
    }

    private fun arrow(glyph: String, onClick: () -> Unit): TextView = TextView(host.context).apply {
        text = glyph
        setTextColor(ink)
        textSize = 18f
        gravity = Gravity.CENTER
        // A thumb's width, whatever the glyph's.
        minWidth = dp(44)
        minHeight = dp(40)
        isSoundEffectsEnabled = false
        setOnClickListener { onClick() }
    }

    private fun paint(row: Choice) {
        valueViews[row]?.text = row.values.getOrElse(row.index) { "" }
    }

    private fun select(at: Int) {
        val rows = page?.rows ?: return
        if (rows.isEmpty()) return
        selected = at.coerceIn(0, rows.lastIndex)
        rowViews.forEachIndexed { index, line ->
            line.background = if (index != selected) null else GradientDrawable().apply {
                setColor(Color.argb(70, 255, 122, 26))
                cornerRadius = dp(8).toFloat()
                setStroke(dp(1), accent)
            }
        }
        // Kept in sight on a page longer than the screen.
        val line = rowViews[selected]
        scroll.post {
            if (line.top < scroll.scrollY) scroll.smoothScrollTo(0, line.top)
            else if (line.bottom > scroll.scrollY + scroll.height) scroll.smoothScrollTo(0, line.bottom - scroll.height)
        }
    }

    // One value along, stopping at either end: a slider's ends, not a wheel.
    private fun step(row: Choice, by: Int) {
        val next = (row.index + by).coerceIn(0, row.values.lastIndex)
        if (next == row.index) return
        row.index = next
        paint(row)
        row.set(next)
    }

    private fun move(dx: Int, dy: Int) {
        val rows = page?.rows ?: return
        if (dy != 0) select((selected + dy + rows.size) % rows.size)
        val row = rows.getOrNull(selected)
        if (dx != 0 && row is Choice) step(row, dx)
    }

    private fun take() {
        when (val row = page?.rows?.getOrNull(selected)) {
            is Entry -> row.run()
            // A switch of two values turns over; a longer row has its arrows.
            is Choice -> if (row.values.size == 2) step(row, if (row.index == 0) 1 else -1)
            null -> Unit
        }
    }

    /**
     * A key while the menu is up, from a pad or the device. Every one is the
     * menu's, so that nothing reaches the game through it; the system's own
     * keys (the volume) are left alone.
     */
    fun onKey(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN
        // Held, a direction repeats; taking and going back do not.
        val first = down && event.repeatCount == 0
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> if (down) move(0, -1)
            KeyEvent.KEYCODE_DPAD_DOWN -> if (down) move(0, 1)
            KeyEvent.KEYCODE_DPAD_LEFT -> if (down) move(-1, 0)
            KeyEvent.KEYCODE_DPAD_RIGHT -> if (down) move(1, 0)
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> if (first) take()
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_ESCAPE -> if (first) back()
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> if (first) close()
            else -> return event.isFromPad()
        }
        return true
    }

    /** The D-pad as a hat, which is how many pads send it, and the left stick. */
    fun onMotion(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) return false
        if (event.action != MotionEvent.ACTION_MOVE) return true
        val x = direction(event.getAxisValue(MotionEvent.AXIS_HAT_X), event.getAxisValue(MotionEvent.AXIS_X))
        val y = direction(event.getAxisValue(MotionEvent.AXIS_HAT_Y), event.getAxisValue(MotionEvent.AXIS_Y))
        if (x != pushedX && x != 0) move(x, 0)
        if (y != pushedY && y != 0) move(0, y)
        pushedX = x
        pushedY = y
        return true
    }

    private fun direction(hat: Float, stick: Float): Int {
        val value = if (abs(hat) > 0.5f) hat else stick
        return if (value > 0.6f) 1 else if (value < -0.6f) -1 else 0
    }

    companion object {
        /** Whether a key came from a controller rather than the device's own buttons. */
        fun KeyEvent.isFromPad(): Boolean =
            source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
    }
}
