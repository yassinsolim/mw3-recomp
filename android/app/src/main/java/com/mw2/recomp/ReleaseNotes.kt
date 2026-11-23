package com.mw2.recomp

import android.graphics.Typeface
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.BulletSpan
import android.text.style.LeadingMarginSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan

/**
 * A release's notes as the update dialog shows them. GitHub hands them over
 * as the Markdown they were written in, and shown as they came a list was a
 * column of hyphens with its second lines under them, and a release written
 * with headings or bold showed the signs for those.
 *
 * `read` is the reading alone, with nothing of Android in it, so that it is
 * tested on the PC (ReleaseNotesTest); `styled` is what the dialog shows.
 */
object ReleaseNotes {

    enum class Kind { HEADING, BULLET, TEXT, GAP }

    /** A line of the notes; `level` is how deep a bullet is nested, from 0. */
    data class Line(val kind: Kind, val text: String, val level: Int = 0)

    /** What of the notes is shown at most; they end at a whole line under it. */
    const val LONGEST = 6000

    private val heading = Regex("^#{1,6}\\s+(.*?)\\s*#*$")
    private val bullet = Regex("^(\\s*)[-*+\u2022]\\s+(.*)$")
    private val rule = Regex("^\\s*([-*_])(\\s*\\1){2,}\\s*$")

    /** The line the release workflow writes for the app itself to read. */
    private val versionCode = Regex("^\\s*APK versionCode:\\s*\\d+\\s*$")

    fun read(body: String): List<Line> {
        var text = body.replace("\r\n", "\n").replace('\r', '\n')
        if (text.length > LONGEST) text = text.take(LONGEST).substringBeforeLast('\n') + "\n\u2026"
        val source = text.split('\n').filterNot { versionCode.matches(it) || rule.matches(it) }
        val lines = ArrayList<Line>()
        for ((index, raw) in source.withIndex()) {
            val line = raw.trimEnd()
            if (line.isBlank()) {
                // One gap between paragraphs, none before the first.
                if (lines.isNotEmpty() && lines.last().kind != Kind.GAP) lines += Line(Kind.GAP, "")
                continue
            }
            val asHeading = heading.find(line)
            val asBullet = bullet.find(line)
            lines += when {
                asHeading != null -> Line(Kind.HEADING, inline(asHeading.groupValues[1]))
                asBullet != null ->
                    Line(Kind.BULLET, inline(asBullet.groupValues[2]), if (asBullet.groupValues[1].length >= 2) 1 else 0)
                // A short line of its own with a list straight under it is
                // that list's title, however plainly it was written.
                titles(line, lines.lastOrNull(), source.getOrNull(index + 1)) -> Line(Kind.HEADING, inline(line.trim()))
                else -> Line(Kind.TEXT, inline(line.trim()))
            }
        }
        while (lines.isNotEmpty() && lines.last().kind == Kind.GAP) lines.removeAt(lines.size - 1)
        return lines
    }

    private fun titles(line: String, before: Line?, next: String?): Boolean =
        (before == null || before.kind == Kind.GAP) && next != null && bullet.matches(next) &&
            line.trim().length <= 60 && line.trimEnd().last() !in ".;!?"

    /** A line without Markdown's signs for bold, italics, code, links and its escapes. */
    private fun inline(text: String): String = text
        .replace(Regex("!?\\[([^\\]]*)]\\([^)]*\\)"), "$1")
        .replace(Regex("<br\\s*/?>", RegexOption.IGNORE_CASE), " ")
        .replace(Regex("(\\*\\*|__|~~)(?=\\S)(.+?)(?<=\\S)\\1"), "$2")
        .replace(Regex("(?<![\\w*\\\\])\\*(?=\\S)([^*]+?)(?<=\\S)\\*(?![\\w*])"), "$1")
        .replace(Regex("(?<![\\w\\\\])_(?=\\S)([^_]+?)(?<=\\S)_(?!\\w)"), "$1")
        .replace(Regex("`([^`]*)`"), "$1")
        .replace(Regex("\\\\([*_`#\\[\\]])"), "$1")

    /** The notes with their bullets hung, their titles bold and half a line between paragraphs. */
    fun styled(body: String, density: Float): CharSequence {
        val out = SpannableStringBuilder()
        val gap = (8 * density).toInt()
        for (line in read(body)) {
            val start = out.length
            out.append(line.text).append('\n')
            val end = out.length
            when (line.kind) {
                Kind.HEADING -> out.setSpan(StyleSpan(Typeface.BOLD), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                Kind.BULLET -> {
                    // The indent first: margins are drawn in the order they
                    // were set, and a nested bullet set before its indent
                    // stood at the left edge with only its text moved in.
                    if (line.level > 0) {
                        out.setSpan(
                            LeadingMarginSpan.Standard(2 * gap * line.level), start, end,
                            Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
                        )
                    }
                    out.setSpan(BulletSpan(gap), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                Kind.GAP -> out.setSpan(RelativeSizeSpan(0.5f), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                Kind.TEXT -> Unit
            }
        }
        return out
    }
}
