package com.mw2.recomp

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.KeyEvent
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * One screen over the game at the start of a run, until the player asks not
 * to see it again: how to open the menu, where the settings are, how the
 * on-screen controls look around (while they are on) and how to quit. The
 * game's own screens say none of it: a player with a controller and the
 * on-screen buttons off has no Menu button to find, the way in being Select
 * and Start together, which nobody guesses, and nothing on the glass says
 * that the right half of it turns the view.
 *
 * It is drawn as the menu is (GameMenu.kt) and closed by a touch or by the
 * pad, whose events GameActivity hands it while it is up; the game loads
 * underneath meanwhile.
 */
class GameHelp(context: Context, private val host: FrameLayout, private val prefs: Prefs, private val onClosed: () -> Unit) {

    private val density = context.resources.displayMetrics.density
    private fun dp(value: Int): Int = (value * density).roundToInt()

    private val accent = Color.rgb(255, 122, 26)
    private val ink = Color.rgb(236, 238, 242)
    private val faint = Color.rgb(160, 168, 180)

    private fun line(context: Context, text: Int, size: Float, colour: Int) = TextView(context).apply {
        setText(text)
        textSize = size
        setTextColor(colour)
        setLineSpacing(dp(2).toFloat(), 1f)
        setPadding(0, dp(5), 0, dp(5))
    }

    private fun button(context: Context, text: Int, filled: Boolean, onClick: () -> Unit) = TextView(context).apply {
        setText(text)
        textSize = 15f
        typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
        setTextColor(if (filled) Color.WHITE else ink)
        background = GradientDrawable().apply {
            cornerRadius = dp(10).toFloat()
            if (filled) setColor(accent) else setStroke(dp(1), faint)
        }
        setPadding(dp(18), dp(10), dp(18), dp(10))
        setOnClickListener { onClick() }
    }

    // The whole screen while the help is up, so that no touch reaches the
    // on-screen pad underneath; only its buttons close it.
    private val scrim = FrameLayout(context).apply {
        setBackgroundColor(Color.argb(150, 0, 0, 0))
        isClickable = true
        isSoundEffectsEnabled = false
        val panel = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(Color.argb(240, 20, 20, 23))
                cornerRadius = dp(14).toFloat()
                setStroke(dp(1), Color.argb(200, 255, 122, 26))
            }
            setPadding(dp(20), dp(14), dp(20), dp(14))
            addView(line(context, R.string.game_help_title, 19f, accent).apply { typeface = Typeface.DEFAULT_BOLD })
            addView(line(context, R.string.game_help_menu, 14.5f, ink))
            addView(line(context, R.string.game_help_settings, 14.5f, ink))
            if (prefs.touchControls) addView(line(context, R.string.game_help_touch, 14.5f, ink))
            addView(line(context, R.string.game_help_quit, 14.5f, ink))
            addView(
                LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    setPadding(0, dp(8), 0, dp(4))
                    addView(button(context, R.string.game_help_ok, filled = true) { close(forGood = false) })
                    addView(
                        button(context, R.string.dont_show_again, filled = false) { close(forGood = true) },
                        LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                            .apply { marginStart = dp(12) },
                    )
                },
            )
            addView(line(context, R.string.game_help_pad, 12f, faint))
        }
        addView(panel, FrameLayout.LayoutParams(dp(460), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
    }

    val isOpen: Boolean
        get() = scrim.parent != null

    /** Shows the help, unless the player has asked not to see it again. */
    fun openUnlessSeen() {
        if (prefs.seenGameHelp || isOpen) return
        host.addView(scrim, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    /** Closes the help; [forGood] when the player asked not to see it again. */
    fun close(forGood: Boolean = false) {
        if (forGood) prefs.seenGameHelp = true
        host.removeView(scrim)
        onClosed()
    }

    /**
     * A pad's key while the help is up: A (or B, or Start) closes it, Y
     * closes it for good, on the key's release so that the game under it
     * sees neither half of the press. Every pad key is taken; any other is
     * left to the activity (the back key, the volume).
     */
    fun onKey(event: KeyEvent): Boolean {
        if (!KeyEvent.isGamepadButton(event.keyCode)) return false
        if (event.action == KeyEvent.ACTION_UP) {
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_START -> close(forGood = false)
                KeyEvent.KEYCODE_BUTTON_Y -> close(forGood = true)
            }
        }
        return true
    }
}
