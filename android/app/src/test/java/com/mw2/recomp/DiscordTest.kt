package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Join our Discord entry opens the project's invite, the one the READMEs and the issue forms give. */
class DiscordTest {
    @Test
    fun theUrlIsTheInvite() {
        assertEquals("https://discord.gg/bTKn3Su3sp", Discord.INVITE)
    }

    @Test
    fun theInviteIsAHttpsLinkWithoutSpaces() {
        assertTrue(Discord.INVITE.startsWith("https://discord.gg/"))
        assertTrue(Discord.INVITE.none { it.isWhitespace() })
    }
}
