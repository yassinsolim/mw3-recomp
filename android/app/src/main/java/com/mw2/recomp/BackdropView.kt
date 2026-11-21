package com.mw2.recomp

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import kotlin.math.sin

/**
 * The launcher's backdrop: charcoal smoke top-right, amber glow
 * bottom-left, slow-drifting haze. Pure decoration, drawn once per
 * animation tick (10 fps is plenty) and deliberately cheap.
 *
 * Once the game is installed it is the game's own artwork instead, from the
 * player's disc (DiscArt.kt), still and under a scrim so the menu reads.
 */
class BackdropView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0,
) : View(context, attrs, defStyle) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val artPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val artMatrix = Matrix()
    private var phase = 0f

    /** The game's artwork, or null for the haze. */
    var art: Bitmap? = null
        set(value) {
            if (field === value) return
            field = value
            // A still picture needs no ticks.
            if (value != null) animator.cancel() else if (isAttachedToWindow && !animator.isStarted) animator.start()
            invalidate()
        }

    private val animator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 24000L
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener {
            phase = it.animatedValue as Float
            postInvalidateOnAnimation()
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (art == null && !animator.isStarted) animator.start()
    }

    override fun onDetachedFromWindow() {
        animator.cancel()
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        art?.let {
            drawArt(canvas, it, w, h)
            return
        }
        val t = phase * Math.PI.toFloat() * 2f

        canvas.drawColor(Color.rgb(0x0B, 0x12, 0x20))

        // Grey smoke, top right, breathing slowly.
        paint.shader = RadialGradient(
            w * (0.86f + 0.03f * sin(t).toFloat()),
            h * (0.10f + 0.02f * sin((t * 1.3f).toFloat())),
            w.coerceAtLeast(h) * 0.75f,
            Color.argb(60, 0x4A, 0x5A, 0x80),
            Color.TRANSPARENT,
            Shader.TileMode.CLAMP,
        )
        canvas.drawRect(0f, 0f, w, h, paint)

        // Second wisp, upper middle, offset phase.
        paint.shader = RadialGradient(
            w * (0.45f + 0.04f * sin((t * 0.7f + 2f).toFloat())),
            h * 0.28f,
            w.coerceAtLeast(h) * 0.55f,
            Color.argb(40, 0x2E, 0x3B, 0x5C),
            Color.TRANSPARENT,
            Shader.TileMode.CLAMP,
        )
        canvas.drawRect(0f, 0f, w, h, paint)

        // Amber glow, bottom left, the MW2 launcher's hearth.
        paint.shader = RadialGradient(
            w * (0.06f + 0.02f * sin((t * 0.5f + 4f).toFloat())),
            h * (0.98f - 0.02f * sin((t * 0.6f).toFloat())),
            w.coerceAtLeast(h) * 0.65f,
            Color.argb(70, 0xFF, 0x7A, 0x1A),
            Color.TRANSPARENT,
            Shader.TileMode.CLAMP,
        )
        canvas.drawRect(0f, 0f, w, h, paint)

        // Vignette so entries and pane sit on darkness.
        paint.shader = null
        paint.color = Color.argb(110, 0x00, 0x00, 0x00)
        canvas.drawRect(0f, 0f, w, h * 0.16f, paint)
        canvas.drawRect(0f, h * 0.84f, w, h, paint)
    }

    /**
     * The artwork filling the screen, cropped about its middle on a screen
     * of another shape than its 16:9, then darkened where the launcher
     * writes: the menu's side most, the version line above and the footer
     * below. One disc's picture is near black and another's a bright dust
     * cloud, and the same grey text has to read on both.
     */
    private fun drawArt(canvas: Canvas, art: Bitmap, w: Float, h: Float) {
        val scale = maxOf(w / art.width, h / art.height)
        artMatrix.setScale(scale, scale)
        artMatrix.postTranslate((w - art.width * scale) / 2f, (h - art.height * scale) / 2f)
        canvas.drawBitmap(art, artMatrix, artPaint)

        val navy = { alpha: Int -> Color.argb(alpha, 0x0B, 0x12, 0x20) }
        // The haze leaves its vignette's alpha in the paint, which a shader is drawn through.
        paint.color = Color.BLACK
        paint.shader = LinearGradient(0f, 0f, w * 0.6f, 0f, navy(190), navy(30), Shader.TileMode.CLAMP)
        canvas.drawRect(0f, 0f, w, h, paint)
        val band = { alphas: IntArray -> IntArray(alphas.size) { navy(alphas[it]) } }
        // Each band holds its darkness as far as the text in it goes, then fades.
        paint.shader = LinearGradient(0f, 0f, 0f, h * 0.2f, band(intArrayOf(240, 215, 0)), null, Shader.TileMode.CLAMP)
        canvas.drawRect(0f, 0f, w, h * 0.2f, paint)
        paint.shader = LinearGradient(0f, h * 0.7f, 0f, h, band(intArrayOf(0, 225, 245)), null, Shader.TileMode.CLAMP)
        canvas.drawRect(0f, h * 0.7f, w, h, paint)
        paint.shader = null
    }
}
