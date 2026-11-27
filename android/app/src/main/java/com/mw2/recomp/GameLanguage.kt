package com.mw2.recomp

import java.util.Locale

/**
 * The language the game is told the console is set to, from the phone's
 * (runtime/kernel/misc.cpp, kernel::ConsoleLanguage; docs/languages.md).
 *
 * Only Black Ops III's disc, title update and packs hold the text and voices
 * of several languages, and the title picks one by the dashboard's language
 * and, for Spanish, its locale: English, French, German, Italian, Spanish of
 * Spain or of the Americas, Brazilian Portuguese, Polish and Russian. The
 * other four games' discs are of one language each, so they are told
 * English, as before, whatever the phone says.
 */
object GameLanguage {
    /** The console's XC_LANGUAGE_* for a phone language the Black Ops III disc has. */
    private val languages = mapOf(
        "en" to 1, "de" to 3, "fr" to 4, "es" to 5, "it" to 6, "pt" to 9, "pl" to 11, "ru" to 12,
    )

    /** XC_LOCALE_* by its position: 1 is Australia, 36 the United States. */
    private val locales = listOf(
        "", "AU", "AT", "BE", "BR", "CA", "CL", "CN", "CO", "CZ", "DK", "FI", "FR", "DE", "GR", "HK", "HU", "IN",
        "IE", "IT", "JP", "KR", "MX", "NL", "NZ", "NO", "PL", "PT", "SG", "SK", "ZA", "ES", "SE", "CH", "TW", "GB",
        "US", "RU", "", "TR", "AR", "SA", "IL", "AE",
    )

    private const val ENGLISH = 1
    private const val UNITED_STATES = 36
    private const val MEXICO = 22

    /** XC_LANGUAGE_* and XC_LOCALE_* for [locale]; English and the United States for a language the disc has not. */
    fun console(locale: Locale): Pair<Int, Int> {
        val language = languages[locale.language] ?: return ENGLISH to UNITED_STATES
        val country = locale.country.uppercase(Locale.ROOT)
        val place = locales.indexOf(country).takeIf { country.isNotEmpty() && it > 0 }
        // Latin American Spanish (es-419) and Spanish of a country the console
        // has no locale for are the Americas' Spanish, which the title chooses
        // by a locale of the Americas.
        return language to (place ?: if (locale.language == "es" && country != "ES") MEXICO else UNITED_STATES)
    }

    /** What the runtime is told for this app's game, or null to leave it English. */
    fun forThisGame(locale: Locale = Locale.getDefault()): Pair<Int, Int>? = if (Title.isBo3) console(locale) else null
}
