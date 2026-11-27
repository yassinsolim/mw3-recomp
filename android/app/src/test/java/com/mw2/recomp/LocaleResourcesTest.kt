package com.mw2.recomp

import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.w3c.dom.Element
import java.io.File
import java.util.IllegalFormatException
import java.util.Locale
import javax.xml.parsers.DocumentBuilderFactory

/**
 * Every translation (values-<locale>/ beside values/ in each source set: main
 * and the five games' flavours) against the English it was made from: the
 * same format placeholders, the same %% where the English has one, a name the
 * English has and may be translated, and a text that formats with the
 * arguments the English takes without throwing. A placeholder lost or added
 * in a translation is a crash on that language's phones only, the first
 * time the string is shown.
 */
class LocaleResourcesTest {
    private val sourceSets = File("src").listFiles { file -> File(file, "res/values").isDirectory }.orEmpty().toList()

    /** One string's texts: a string has one, an array one per item, plurals one per quantity. */
    private data class Entry(val texts: List<String>, val translatable: Boolean)

    private fun read(file: File): Map<String, Entry> {
        val document = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(file)
        val entries = LinkedHashMap<String, Entry>()
        val children = document.documentElement.childNodes
        for (i in 0 until children.length) {
            val element = children.item(i) as? Element ?: continue
            val name = element.getAttribute("name")
            val translatable = element.getAttribute("translatable") != "false"
            val texts = when (element.tagName) {
                "string" -> listOf(element.textContent)
                "string-array" -> {
                    val items = element.getElementsByTagName("item")
                    (0 until items.length).map { items.item(it).textContent }
                }
                // "other" last: the quantity with every placeholder, which a translation's items are held to.
                "plurals" -> {
                    val items = element.getElementsByTagName("item")
                    (0 until items.length).map { items.item(it) as Element }
                        .sortedBy { it.getAttribute("quantity") == "other" }.map { it.textContent }
                }
                else -> continue
            }
            entries["${element.tagName}/$name"] = Entry(texts, translatable)
        }
        return entries
    }

    // Java's conversions, without the space flag: "75% is" in a sentence that is never formatted is a percent
    // sign and a word, not "% i". A lone % is compared as a count (percents).
    private val token = Regex("""%%|%(\d+\$)?[-#+0,(]*\d*(\.\d+)?[bBhHsScCdoxXeEfgGaA]""")

    private fun placeholders(text: String) = token.findAll(text).map { it.value }.filter { it != "%%" }.sorted().toList()

    private fun percents(text: String) = token.findAll(text).count { it.value == "%%" } to token.replace(text, "").count { it == '%' }

    /** Arguments of the kinds the English's placeholders take, in their positions. */
    private fun argumentsFor(english: String): Array<Any> {
        val kinds = sortedMapOf<Int, Char>()
        var next = 1
        token.findAll(english).filter { it.value != "%%" }.forEach { match ->
            val position = match.groupValues[1].removeSuffix("$").toIntOrNull() ?: next++
            kinds[position] = match.value.last()
        }
        val count = kinds.keys.maxOrNull() ?: 0
        return Array(count) { index ->
            when (kinds[index + 1]?.lowercaseChar()) {
                'd', 'x', 'o' -> 7
                'f', 'e', 'g' -> 1.5
                else -> "text"
            }
        }
    }

    @Test
    fun everyTranslationKeepsTheEnglishPlaceholders() {
        val problems = mutableListOf<String>()
        var locales = 0
        var checked = 0
        for (set in sourceSets) {
            val translated = File(set, "res").listFiles { file -> file.isDirectory && file.name.startsWith("values-") }.orEmpty()
            for (folder in translated) {
                val language = folder.name.removePrefix("values-").substringBefore("-")
                val files = folder.listFiles { file -> file.name.startsWith("strings") && file.extension == "xml" }.orEmpty()
                if (files.isNotEmpty() && set.name == "main") locales++
                for (file in files) {
                    val english = read(File(set, "res/values/${file.name}"))
                    for ((name, entry) in read(file)) {
                        val where = "${set.name}/${folder.name}/${file.name} $name"
                        val source = english[name]
                        if (source == null) { problems += "$where: not in values/${file.name}"; continue }
                        if (!source.translatable) { problems += "$where: the English is translatable=\"false\""; continue }
                        // Plurals are compared with the English "other", the one with every placeholder.
                        val englishText = if (name.startsWith("plurals/")) source.texts.last() else null
                        entry.texts.forEachIndexed { index, text ->
                            val from = englishText ?: source.texts.getOrNull(index)
                            if (from == null) { problems += "$where: item ${index + 1} has no English"; return@forEachIndexed }
                            checked++
                            if (placeholders(text) != placeholders(from)) problems += "$where: ${placeholders(text)} for the English ${placeholders(from)}"
                            if (percents(text) != percents(from)) problems += "$where: its % signs differ from the English's"
                            if (placeholders(from).isNotEmpty()) {
                                try {
                                    String.format(Locale.forLanguageTag(language), text, *argumentsFor(from))
                                } catch (failure: IllegalFormatException) {
                                    problems += "$where: does not format: $failure"
                                }
                            }
                        }
                    }
                }
            }
        }
        if (problems.isNotEmpty()) fail(problems.joinToString("\n", "${problems.size} translations differ from their English:\n"))
        assertTrue("no translations were found under ${File("src").absolutePath}", locales > 0 && checked > 0)
    }
}
