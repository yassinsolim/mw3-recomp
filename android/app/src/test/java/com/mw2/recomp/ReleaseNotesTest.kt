package com.mw2.recomp

import com.mw2.recomp.ReleaseNotes.Kind
import com.mw2.recomp.ReleaseNotes.Line
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ReleaseNotes.read on notes in the forms the releases have had (plain lines
 * with hyphen lists under plain titles) and in the Markdown a later one may
 * be written in.
 */
class ReleaseNotesTest {
    @Test
    fun aPlainListUnderATitle() {
        val notes = "How to get it\n- Your own disc.\n- A PC.\n\nNever share it. Really.\n"
        assertEquals(
            listOf(
                Line(Kind.HEADING, "How to get it"),
                Line(Kind.BULLET, "Your own disc."),
                Line(Kind.BULLET, "A PC."),
                Line(Kind.GAP, ""),
                Line(Kind.TEXT, "Never share it. Really."),
            ),
            ReleaseNotes.read(notes),
        )
    }

    @Test
    fun aSentenceBeforeAListIsNotItsTitle() {
        val lines = ReleaseNotes.read("This release mends two things.\n- One.\n- Two.")
        assertEquals(Kind.TEXT, lines[0].kind)
    }

    @Test
    fun markdownSignsAreTakenOut() {
        val notes = "## What's new\r\n\r\n* **Online**: uses `MW2_NAME` and [the relay](https://example.org/x).\r\n" +
            "  - nested, with *stress* and a file_name_kept and 2 * 3 * 4\r\n" +
            "---\r\n\r\n\r\nRun setup\\setup.cmd \\*once\\*.\r\nAPK versionCode: 106\r\n"
        assertEquals(
            listOf(
                Line(Kind.HEADING, "What's new"),
                Line(Kind.GAP, ""),
                Line(Kind.BULLET, "Online: uses MW2_NAME and the relay."),
                Line(Kind.BULLET, "nested, with stress and a file_name_kept and 2 * 3 * 4", 1),
                Line(Kind.GAP, ""),
                Line(Kind.TEXT, "Run setup\\setup.cmd *once*."),
            ),
            ReleaseNotes.read(notes),
        )
    }

    @Test
    fun longNotesEndAtAWholeLine() {
        val lines = ReleaseNotes.read((1..2000).joinToString("\n") { "- line number $it" })
        assertEquals("\u2026", lines.last().text)
        assertTrue(lines[lines.size - 2].text.matches(Regex("line number \\d+")))
        assertTrue(lines.sumOf { it.text.length } < ReleaseNotes.LONGEST)
    }
}
