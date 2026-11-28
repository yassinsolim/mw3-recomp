package com.mw2.recomp

import org.json.JSONArray
import org.json.JSONObject

/**
 * The phone's graphics chip, as far as choosing a driver needs it: whose it
 * is and its number. Read from the line DeviceInfo.gpu writes, which is the
 * OpenGL ES driver's, so that asking does not open the Vulkan driver that
 * may be the trouble.
 */
data class Gpu(val vendor: String, val model: Int, val name: String) {
    companion object {
        const val ADRENO = "adreno"
        const val MALI = "mali"
        const val XCLIPSE = "xclipse"
        const val POWERVR = "powervr"
        const val OTHER = "other"

        private val adreno = Regex("""Adreno \(TM\) (\d{3})\b""")
        private val mali = Regex("""(?:Mali|Immortalis)-[GT](\d{2,4})""")
        private val xclipse = Regex("""Xclipse (\d{3,4})""")

        /**
         * [text] is "Adreno (TM) 740 (Qualcomm), OpenGL ES 3.2 V@0676.42 ...",
         * "Mali-G715 (ARM), ...", "Xclipse 940 (Samsung ...)". An Adreno
         * without a three-digit number (the 7c+ Gen 3, the 8cx) has model 0:
         * no driver in the catalog is offered for a chip it cannot name.
         */
        fun parse(text: String): Gpu {
            adreno.find(text)?.let { return Gpu(ADRENO, it.groupValues[1].toInt(), "Adreno ${it.groupValues[1]}") }
            mali.find(text)?.let { return Gpu(MALI, it.groupValues[1].toInt(), it.value) }
            xclipse.find(text)?.let { return Gpu(XCLIPSE, it.groupValues[1].toInt(), it.value) }
            return when {
                text.contains("Adreno", ignoreCase = true) -> Gpu(ADRENO, 0, "Adreno")
                text.contains("PowerVR", ignoreCase = true) -> Gpu(POWERVR, 0, "PowerVR")
                else -> Gpu(OTHER, 0, "")
            }
        }
    }
}

/**
 * The driver catalog (android/drivers/catalog.json, docs/android.md "The
 * driver catalog"): which driver packages exist for which chips, where each
 * is published, its SHA-256, and what is known of it from this project's
 * tests and players' logs. The app carries the catalog it was built with and
 * takes a newer one from the project's release when there is one
 * (DriverAuto.kt).
 *
 * A driver is native code that runs inside the app. So: nothing is ever
 * downloaded whose SHA-256 is not in the catalog, a URL is only taken when
 * it is a GitHub release asset over HTTPS ([allowedUrl]), and an entry that
 * is not marked `auto` -- Qualcomm's own drivers repackaged by others, which
 * the project links to and does not carry, and experimental builds -- is
 * only ever fetched when the player asks for it by name.
 */
class DriverCatalog(val version: Int, val drivers: List<Entry>, val system: List<SystemRule>) {

    data class GpuRange(val vendor: String, val from: Int, val to: Int) {
        fun holds(gpu: Gpu): Boolean = gpu.vendor == vendor && gpu.model in from..to
    }

    /**
     * What a test or a player's log said. [gpus] empty: every chip of the
     * entry. [game] "*": every game. [driver]: for the phone's own driver,
     * a piece of its version as OpenGL ES names it ("V@0800.71"); empty for
     * any version.
     */
    data class Report(val gpus: List<GpuRange>, val game: String, val good: Boolean, val driver: String, val source: String) {
        fun about(gpu: Gpu, game: String, own: List<GpuRange>, glText: String): Boolean =
            (this.game == "*" || this.game == game) &&
                (if (gpus.isEmpty()) own.any { it.holds(gpu) } else gpus.any { it.holds(gpu) }) &&
                (driver.isEmpty() || glText.contains(driver))
    }

    data class Entry(
        val id: String,
        val name: String,
        val version: String,
        val kind: String,
        val source: String,
        /** Where it is published, or "" for one only this app carries. */
        val url: String,
        /** The app's own copy under assets/, or "". */
        val asset: String,
        val sha256: String,
        val size: Long,
        val library: String,
        val licence: String,
        /** Whether the app may choose it by itself. */
        val auto: Boolean,
        val minApi: Int,
        val gpus: List<GpuRange>,
        val rank: Int,
        val rankFor: List<Pair<List<GpuRange>, Int>>,
        val reports: List<Report>,
        val note: String,
    ) {
        fun fits(gpu: Gpu, sdk: Int): Boolean = sdk >= minApi && gpus.any { it.holds(gpu) }

        fun rankOn(gpu: Gpu): Int = rankFor.firstOrNull { (ranges, _) -> ranges.any { it.holds(gpu) } }?.second ?: rank
    }

    data class SystemRule(val gpus: List<GpuRange>, val rank: Int, val reports: List<Report>, val note: String)

    /**
     * One driver the phone could run the game on, and how it compares:
     * [entry] null is the phone's own driver. [knownGood]: a report says it
     * ran this game on this chip.
     */
    data class Choice(val entry: Entry?, val score: Int, val knownGood: Boolean) {
        val id: String get() = entry?.id ?: SYSTEM
    }

    /**
     * The drivers to try on this phone, best first.
     *
     * In: every catalog driver for the chip and the Android version that the
     * app may choose by itself ([autoOnly]), and the phone's own driver.
     * Out: any a report calls broken for this chip and game, and any in
     * [failed] (what this phone's own self-test or first frames refused).
     * Order: one a report calls good for this chip and game before one
     * nothing is known of, then by rank, then as the catalog lists them. The
     * phone's own driver is ranked by the catalog's rule for the chip; where
     * no rule names the chip it is the only choice there is.
     */
    fun ranked(gpu: Gpu, sdk: Int, game: String, glText: String, failed: Set<String>, autoOnly: Boolean = true): List<Choice> {
        val choices = ArrayList<Choice>()
        for (entry in drivers) {
            if (!entry.fits(gpu, sdk) || (autoOnly && !entry.auto) || entry.id in failed) continue
            val about = entry.reports.filter { it.about(gpu, game, entry.gpus, glText) }
            if (about.any { !it.good }) continue
            val good = about.any { it.good }
            choices.add(Choice(entry, entry.rankOn(gpu) + if (good) KNOWN_GOOD else 0, good))
        }
        if (SYSTEM !in failed) {
            val rule = system.firstOrNull { rule -> rule.gpus.any { it.holds(gpu) } }
            val about = rule?.reports.orEmpty().filter { it.about(gpu, game, rule!!.gpus, glText) }
            if (about.none { !it.good }) {
                val good = about.any { it.good }
                choices.add(Choice(null, (rule?.rank ?: 0) + if (good) KNOWN_GOOD else 0, good))
            }
        }
        // sortedByDescending is stable: equal scores keep the catalog's order.
        return choices.sortedByDescending { it.score }
    }

    fun entry(id: String): Entry? = drivers.firstOrNull { it.id == id }

    /**
     * The report that calls the driver [id] (an entry's, or [SYSTEM] for the
     * phone's own, whose version [glText] names) broken on this chip in this
     * game, or null: DriverRescue asks a player who chose it.
     */
    fun brokenHere(id: String, gpu: Gpu, game: String, glText: String): Report? {
        val (reports, own) = if (id == SYSTEM) {
            val rule = system.firstOrNull { rule -> rule.gpus.any { it.holds(gpu) } } ?: return null
            rule.reports to rule.gpus
        } else {
            val entry = entry(id) ?: return null
            entry.reports to entry.gpus
        }
        return reports.firstOrNull { !it.good && it.about(gpu, game, own, glText) }
    }

    /** The entry a package with this SHA-256 is, if the catalog knows it. */
    fun bySha(sha256: String): Entry? = drivers.firstOrNull { it.sha256 == sha256.lowercase() }

    companion object {
        const val SCHEMA = 1
        const val SYSTEM = "system"
        private const val KNOWN_GOOD = 1000

        /** The game as the catalog's reports name it. */
        val game: String = when {
            Title.isBo3 -> "bo3"
            Title.isMw3 -> "mw3"
            Title.isCod4 -> "cod4"
            Title.isAw -> "aw"
            else -> "mw2"
        }

        private val releaseAsset = Regex("""https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[^?#\s]+""")
        private val sha = Regex("[0-9a-f]{64}")
        private val ident = Regex("[a-z0-9][a-z0-9._-]{2,63}")

        /** Whether a catalog's URL is one the app downloads from: a GitHub release asset, over HTTPS. */
        fun allowedUrl(url: String): Boolean = releaseAsset.matches(url)

        /**
         * Reads a catalog, or null when it is not one this app understands.
         * An entry that is not whole -- no SHA-256, a URL from elsewhere, a
         * proprietary driver marked for automatic choice -- is left out
         * rather than trusted in part.
         */
        fun parse(text: String): DriverCatalog? = try {
            val json = JSONObject(text)
            if (json.optInt("schema") != SCHEMA || json.optInt("version") <= 0) null
            else DriverCatalog(
                json.getInt("version"),
                list(json.optJSONArray("drivers")).mapNotNull(::entry),
                list(json.optJSONArray("system")).map {
                    SystemRule(ranges(it.optJSONArray("gpus")), it.optInt("rank"), reports(it.optJSONArray("reports")), it.optString("note"))
                },
            )
        } catch (_: Exception) {
            null
        }

        private fun list(array: JSONArray?): List<JSONObject> =
            if (array == null) emptyList() else (0 until array.length()).mapNotNull { array.optJSONObject(it) }

        private fun ranges(array: JSONArray?): List<GpuRange> =
            list(array).map { GpuRange(it.optString("vendor"), it.optInt("from"), it.optInt("to")) }

        private fun reports(array: JSONArray?): List<Report> = list(array).mapNotNull {
            val result = it.optString("result")
            if (result != "good" && result != "broken") null
            else Report(ranges(it.optJSONArray("gpus")), it.optString("game", "*"), result == "good", it.optString("driver"), it.optString("source"))
        }

        private fun entry(json: JSONObject): Entry? {
            val id = json.optString("id")
            val url = json.optString("url")
            val asset = json.optString("asset")
            val hash = json.optString("sha256")
            val kind = json.optString("kind")
            val auto = json.optBoolean("auto", false)
            val library = json.optString("library")
            if (!ident.matches(id) || !sha.matches(hash)) return null
            if (url.isEmpty() && asset.isEmpty()) return null
            if (url.isNotEmpty() && !allowedUrl(url)) return null
            if (asset.isNotEmpty() && !Regex("drivers/[A-Za-z0-9_.-]+\\.zip").matches(asset)) return null
            if (library.isEmpty() || library.contains('/')) return null
            if (kind == "qualcomm" && (auto || asset.isNotEmpty())) return null
            return Entry(
                id = id,
                name = json.optString("name", id),
                version = json.optString("version"),
                kind = kind,
                source = json.optString("source"),
                url = url,
                asset = asset,
                sha256 = hash,
                size = json.optLong("size"),
                library = library,
                licence = json.optString("licence"),
                auto = auto,
                minApi = json.optInt("minApi", 26),
                gpus = ranges(json.optJSONArray("gpus")),
                rank = json.optInt("rank"),
                rankFor = list(json.optJSONArray("rankFor")).map { ranges(it.optJSONArray("gpus")) to it.optInt("rank") },
                reports = reports(json.optJSONArray("reports")),
                note = json.optString("note"),
            )
        }
    }
}
