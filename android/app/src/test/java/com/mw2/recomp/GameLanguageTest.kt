package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.Locale

/** GameLanguage.console: the phone's language as the dashboard's language and locale. */
class GameLanguageTest {
    private fun console(tag: String) = GameLanguage.console(Locale.forLanguageTag(tag))

    @Test
    fun theDiscsLanguages() {
        assertEquals(1 to 36, console("en-US"))
        assertEquals(1 to 35, console("en-GB"))
        assertEquals(3 to 13, console("de-DE"))
        assertEquals(3 to 2, console("de-AT"))
        assertEquals(4 to 12, console("fr-FR"))
        assertEquals(4 to 5, console("fr-CA"))
        assertEquals(6 to 19, console("it-IT"))
        assertEquals(9 to 4, console("pt-BR"))
        assertEquals(9 to 27, console("pt-PT"))
        assertEquals(11 to 26, console("pl-PL"))
        assertEquals(12 to 37, console("ru-RU"))
    }

    /** The title takes Spanish with a locale of the Americas for the Americas' Spanish (docs/languages.md). */
    @Test
    fun spanishOfSpainAndOfTheAmericas() {
        assertEquals(5 to 31, console("es-ES"))
        assertEquals(5 to 22, console("es-MX"))
        assertEquals(5 to 40, console("es-AR"))
        assertEquals(5 to 22, console("es-419"))
        assertEquals(5 to 22, console("es-PE"))
    }

    @Test
    fun aLanguageTheDiscHasNotIsEnglish() {
        for (tag in listOf("ja-JP", "ko-KR", "zh-CN", "ar-SA", "tr-TR", "in-ID", "nl-NL")) {
            assertEquals(tag, 1 to 36, console(tag))
        }
    }

    @Test
    fun aRussianWithoutACountryIsInTheUnitedStates() {
        assertEquals(12 to 36, console("ru"))
    }
}
