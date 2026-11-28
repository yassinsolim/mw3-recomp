package com.mw2.recomp

/**
 * The compile before the first frame that Compile shaders at boot asks for
 * (MW2_SHADER_PREWARM_ALL), as the runtime reports it in
 * [NativeBridge.nativeStats]: [total] pipelines, [done] of them gone through,
 * and whether what it builds is [kept] for the next run.
 *
 * On a Galaxy A35 (Mali-G68) it was 1,064 pipelines one at a time, three to
 * five minutes of a black screen with nothing on it, which its player took
 * for a freeze (mw2 #4). The card over the screen (GameActivity) says how
 * far it is, and whether the wait comes back: where the driver's cache is
 * kept the next start finds every pipeline in it, and where it is not
 * (a Mali driver older than r38, pipeline_cache_rules.h) every start
 * compiles them all again.
 */
data class BootCompile(val total: Int, val done: Int, val kept: Boolean) {
    /** Still compiling: the card shows while this holds and no frame has reached the screen. */
    val running: Boolean get() = total > 0 && done < total

    val percent: Int get() = if (total > 0) done.coerceIn(0, total) * 100 / total else 0

    /** The sentence under the count: a one-time wait, or one at every start. */
    val note: Int get() = if (kept) R.string.boot_compile_once else R.string.boot_compile_every_start

    companion object {
        /** From [NativeBridge.nativeStats]'s values; null from a runtime that does not report it. */
        fun from(values: FloatArray): BootCompile? =
            if (values.size < STATS_VALUES) null
            else BootCompile(values[19].toInt(), values[20].toInt(), values[21] > 0.5f)

        /** How many values [NativeBridge.nativeStats] fills, with these. */
        const val STATS_VALUES = 22
    }
}
