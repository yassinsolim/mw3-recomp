package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Join our Discord entry opens the project's Discord link, the one the READMEs and the issue forms give. */
class DiscordTest {
    @Test
    fun theUrlIsTheProjectsDiscordLink() {
        assertEquals("https://yassin.app/discord", Discord.URL)
    }

    @Test
    fun theUrlIsAHttpsLinkWithoutSpaces() {
        assertTrue(Discord.URL.startsWith("https://yassin.app/"))
        assertTrue(Discord.URL.none { it.isWhitespace() })
    }
}
