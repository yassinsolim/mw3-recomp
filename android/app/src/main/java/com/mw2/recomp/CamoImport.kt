package com.mw2.recomp

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * A PC camouflage mod made into a camouflage pack on the phone, as
 * tools/import_pack.py makes one on a PC (docs/textures.md, "Camouflage
 * packs"): the same rules, so a pack is the same whichever made it.
 *
 * A PC mod is a set of IWI images named after the game's image each one
 * replaces. Nothing is replaced here: every such image becomes a camouflage
 * of its own, listed below Gold. A mod's Gold (`detail_gold_col`,
 * `~detail_gold_spc-...`, `weapon_camo_menu_gold[_sm]`) is one camouflage; its
 * replacement of the game's `weapon_camo_<x>` another; and whole-weapon skins
 * (`weapon_<gun>_col`) together are one, with a tile cut from each weapon's
 * skin. Pictures that are not named after an image of the game's are each a
 * camouflage when the mod has none of those.
 *
 * Everything here but [write] is plain Kotlin, so the rules are tested on
 * the build machine (CamoImportTest).
 */
object CamoImport {
    class Refused(reason: String) : Exception(reason)

    /** A decoded picture: ARGB, row by row. */
    class Picture(val width: Int, val height: Int, val argb: IntArray)

    /** A file of the mod: its path inside it, lower case with forward slashes, and its bytes when asked. */
    class Source(val name: String, val read: () -> ByteArray)

    /** What the project knows of a pack (setup/camo-packs.json), by the mod's file name. */
    class Known(
        val id: String,
        val name: String,
        val label: String,
        val author: String,
        val url: String,
        val names: Map<String, String>,
    )

    /** A camouflage found in a mod, before its pictures are read. */
    class Slot(
        val id: String,
        val name: String,
        val target: String,
        val maps: Map<String, Source>,
        val weapons: Map<String, Source> = emptyMap(),
        val frames: List<Source> = emptyList(),
    )

    const val SIZE = 256
    const val SWATCH = 128
    const val BANNER_WIDTH = 408
    const val BANNER_HEIGHT = 152
    private val PICTURES = listOf(".png", ".jpg", ".jpeg", ".bmp", ".webp")

    // The game's camouflages by the name its images carry, as the menu calls them.
    private val STOCK = mapOf(
        "classic" to "Classic", "snow" to "Snow", "multi" to "Multicam", "d_urban" to "Digital Urban",
        "hex" to "Hex", "choco" to "Choco", "snake" to "Snake", "blue" to "Blue", "red" to "Red",
        "autumn" to "Autumn", "gold" to "Gold", "marine" to "Marine", "winter" to "Winter",
    )

    // A weapon's skin image by the weapon a class names (iw5_<x>): the images'
    // names are older than the weapons' and differ where listed.
    private val SKIN_WEAPON = mapOf(
        "m4_spr" to "m4", "remington_acr" to "acr", "remington_msr" to "msr", "l_96_a1" to "l96a1",
        "m14_ebr" to "mk14", "uzi_m9" to "m9", "g36" to "g36c", "desert_eagle" to "deserteagle",
        "44_magnum" to "44magnum", "spas_12" to "spas12", "m82" to "barrett", "fmg" to "fmg9",
        "scar_l" to "scar", "mp5_new" to "mp5",
    )

    fun isPicture(name: String): Boolean {
        val lower = name.lowercase()
        return lower.endsWith(".iwi") || PICTURES.any { lower.endsWith(it) }
    }

    // ---- IWI (IW5, version 8): a 32-byte header, mip levels smallest first ----

    fun decodeIwi(data: ByteArray): Picture {
        if (data.size < 32 || data[0] != 'I'.code.toByte() || data[1] != 'W'.code.toByte() || data[2] != 'i'.code.toByte()) {
            throw Refused("not an IWi file")
        }
        if (data[3].toInt() != 8) throw Refused("IWi version ${data[3]} (only Modern Warfare 3's, 8, is read)")
        fun u8(at: Int) = data[at].toInt() and 0xFF
        fun u16(at: Int) = u8(at) or (u8(at + 1) shl 8)
        val flags = u8(4) or (u8(5) shl 8) or (u8(6) shl 16) or (u8(7) shl 24)
        if (flags and 0x30000 != 0) throw Refused("a cube or a volume")
        val format = u8(8)
        val width = u16(10)
        val height = u16(12)
        if (width == 0 || height == 0 || width > 8192 || height > 8192) throw Refused("an image of no size")
        val block = when (format) { 0xB -> 8; 0xC, 0xD -> 16; else -> 0 }
        val perTexel = when (format) { 1 -> 4; 2 -> 3; 3 -> 2; 4, 5 -> 1; else -> 0 }
        if (block == 0 && perTexel == 0) throw Refused(if (format == 0xE) "a normal map" else "a format that is not decoded")
        val size = if (block != 0) ((width + 3) / 4) * ((height + 3) / 4) * block else width * height * perTexel
        // The largest level is the file's last bytes.
        val at = data.size - size
        if (at < 32) throw Refused("too short for its ${width}x$height picture")
        val out = IntArray(width * height)
        if (block != 0) {
            decodeDxt(data, at, width, height, format, out)
        } else {
            for (i in 0 until width * height) {
                val p = at + i * perTexel
                out[i] = when (format) {
                    1 -> (u8(p + 3) shl 24) or (u8(p + 2) shl 16) or (u8(p + 1) shl 8) or u8(p)
                    2 -> (0xFF shl 24) or (u8(p + 2) shl 16) or (u8(p + 1) shl 8) or u8(p)
                    3 -> (u8(p + 1) shl 24) or (u8(p) * 0x010101)
                    else -> (0xFF shl 24) or (u8(p) * 0x010101)
                }
            }
        }
        return Picture(width, height, out)
    }

    private fun decodeDxt(data: ByteArray, start: Int, width: Int, height: Int, format: Int, out: IntArray) {
        fun u8(at: Int) = data[at].toInt() and 0xFF
        val stride = if (format == 0xB) 8 else 16
        val colours = IntArray(4)
        val alphas = IntArray(8)
        var at = start
        for (by in 0 until (height + 3) / 4) {
            for (bx in 0 until (width + 3) / 4) {
                val c = at + stride - 8
                val c0 = u8(c) or (u8(c + 1) shl 8)
                val c1 = u8(c + 2) or (u8(c + 3) shl 8)
                fun channel(value: Int, bits: Int) = (value shl (8 - bits)) or (value shr (2 * bits - 8))
                val r0 = channel(c0 shr 11 and 31, 5); val g0 = channel(c0 shr 5 and 63, 6); val b0 = channel(c0 and 31, 5)
                val r1 = channel(c1 shr 11 and 31, 5); val g1 = channel(c1 shr 5 and 63, 6); val b1 = channel(c1 and 31, 5)
                fun rgb(r: Int, g: Int, b: Int) = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
                colours[0] = rgb(r0, g0, b0)
                colours[1] = rgb(r1, g1, b1)
                if (format != 0xB || c0 > c1) {
                    colours[2] = rgb((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3)
                    colours[3] = rgb((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3)
                } else {
                    colours[2] = rgb((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2)
                    colours[3] = 0
                }
                val bits = u8(c + 4) or (u8(c + 5) shl 8) or (u8(c + 6) shl 16) or (u8(c + 7) shl 24)
                var alphaBits = 0L
                if (format == 0xD) {
                    val a0 = u8(at)
                    val a1 = u8(at + 1)
                    alphas[0] = a0
                    alphas[1] = a1
                    for (k in 1..6) {
                        alphas[k + 1] = if (a0 > a1) ((7 - k) * a0 + k * a1) / 7
                        else if (k <= 4) ((5 - k) * a0 + k * a1) / 5 else if (k == 5) 0 else 255
                    }
                    for (i in 0 until 6) alphaBits = alphaBits or (u8(at + 2 + i).toLong() shl (8 * i))
                }
                for (i in 0 until 16) {
                    val x = bx * 4 + (i and 3)
                    val y = by * 4 + (i shr 2)
                    if (x >= width || y >= height) continue
                    var texel = colours[bits ushr (2 * i) and 3]
                    if (format == 0xC) {
                        val nibble = u8(at + i / 2) shr (4 * (i and 1)) and 15
                        texel = (texel and 0xFFFFFF) or ((nibble * 17) shl 24)
                    } else if (format == 0xD) {
                        texel = (texel and 0xFFFFFF) or (alphas[(alphaBits ushr (3 * i) and 7).toInt()] shl 24)
                    }
                    out[y * width + x] = texel
                }
                at += stride
            }
        }
    }

    // ---- what a file of the mod is ----

    /** (kind, target, role) for an image named after one of the game's; null for any other name. */
    fun classify(stem: String): Triple<String, String, String>? {
        Regex("^~?detail_gold_(col|c|spc|spec|s)\\b.*").matchEntire(stem)?.let {
            return Triple("camo", "gold", if (it.groupValues[1].startsWith("c")) "colour" else "specular")
        }
        Regex("^weapon_camo_menu_(.+?)(_sm)?$").matchEntire(stem)?.let {
            return Triple("camo", it.groupValues[1], if (it.groupValues[2].isEmpty()) "banner" else "swatch")
        }
        Regex("^(~?)weapon_camo_([a-z0-9_]+?)(?:-[a-z0-9]*rgb.*)?$").matchEntire(stem)?.let {
            return Triple("camo", it.groupValues[2], if (it.groupValues[1].isEmpty()) "colour" else "specular")
        }
        Regex("^(~?)(?:weapon_)?([a-z0-9_]+?)_(col|spc|spec|nml|nrml|cos)(?:-.*|~.*)?$").matchEntire(stem)?.let {
            val derived = it.groupValues[1] == "~"
            val usage = it.groupValues[3]
            var gun = it.groupValues[2]
            if (usage in listOf("nml", "nrml", "cos") || "scope" in gun) return Triple("skin", gun, "skip")
            if (derived == (usage == "spc" || usage == "spec")) {
                gun = gun.removeSuffix("_iw5")
                return Triple("skin", "iw5_" + (SKIN_WEAPON[gun] ?: gun.replace("_", "")), if (derived) "specular" else "colour")
            }
        }
        return null
    }

    fun ident(name: String) = name.lowercase().replace(Regex("[^a-z0-9]+"), "-").trim('-')

    fun title(name: String) =
        name.lowercase().split(Regex("[^a-z0-9]+")).filter { it.isNotEmpty() }
            .joinToString(" ") { it.replaceFirstChar(Char::uppercase) }

    /** A free picture's name as the camouflage it is, or null for a technical map. */
    private fun tidy(stem: String): String? {
        val name = stem.substringAfterLast('/')
        if (Regex(".*(_n|_nml|_nrml|_nrm|_normal|_s|_spc|_spec|_cos|_g|_gloss|_o|_occ)$").matches(name)) return null
        return name.replace(Regex("^(weapon_camo_|weapon_|camo_)"), "").ifEmpty { null }
    }

    /** The camouflages of a mod, from its pictures, as tools/import_pack.py's collect finds them. */
    fun collect(files: List<Source>, packName: String, names: Map<String, String>): List<Slot> {
        class Found(val naming: Int, val folder: String, val leaf: String, val maps: MutableMap<String, Source>)

        val slots = sortedMapOf<Pair<String, String>, MutableMap<String, Source>>(compareBy({ it.first }, { it.second }))
        val skins = sortedMapOf<String, MutableMap<String, Source>>()
        val free = mutableListOf<Source>()
        for (file in files.sortedBy { it.name }) {
            val stem = file.name.substringBeforeLast('.')
            val folder = stem.substringBeforeLast('/', "")
            val what = classify(stem.substringAfterLast('/'))
            when {
                what == null -> free += file
                what.first == "camo" -> slots.getOrPut(folder to what.second) { mutableMapOf() }.putIfAbsent(what.third, file)
                what.third != "skip" -> skins.getOrPut(what.second) { mutableMapOf() }.putIfAbsent(what.third, file)
            }
        }

        // One entry for each distinct content of a target; of the folders that
        // have the same content, the one that names it (not `All`) is kept.
        val byContent = LinkedHashMap<Pair<String, String>, Found>()
        for ((key, maps) in slots) {
            val (folder, target) = key
            if (maps["specular"] == null && maps["colour"] == null) continue
            val digest = MessageDigest.getInstance("SHA-256")
            for (role in listOf("colour", "specular")) maps[role]?.let { digest.update(it.read()) }
            val content = target to digest.digest().joinToString("") { "%02x".format(it) }
            val leaf = folder.substringAfterLast('/')
            val naming = (if (" - " in leaf) 2 else 0) + (if (leaf !in listOf("all", "images", "")) 1 else 0)
            val have = byContent[content]
            if (have == null || naming > have.naming) {
                val merged = have?.maps ?: mutableMapOf()
                merged.putAll(maps)
                byContent[content] = Found(naming, folder, leaf, merged)
            } else {
                for ((role, source) in maps) have.maps.putIfAbsent(role, source)
            }
        }
        val variants = byContent.keys.groupingBy { it.first }.eachCount()
        val single = byContent.size + (if (skins.isEmpty()) 0 else 1) == 1
        val out = mutableListOf<Slot>()
        for ((content, found) in byContent.entries.sortedBy { it.value.folder }) {
            val target = content.first
            val said = Regex("^.+?\\s+-\\s+(.+)$").matchEntire(found.leaf)
            val name = names[target]
                ?: said?.let { title(it.groupValues[1]) }
                ?: if (single) packName
                else if (variants.getValue(target) > 1 && found.leaf.isNotEmpty()) title(found.leaf)
                else STOCK[target] ?: title(target)
            val id = if (variants.getValue(target) == 1) ident(target) else ident(found.leaf + "-" + target)
            out += Slot(id, name, if (target == "gold") "gold" else "weapon_camo_$target", found.maps)
        }
        if (skins.isNotEmpty()) {
            val weapons = skins.mapNotNull { (weapon, maps) -> (maps["specular"] ?: maps["colour"])?.let { weapon to it } }.toMap()
            out += Slot(if (out.isEmpty()) ident(packName).ifEmpty { "skin" } else "skin", names["skin"] ?: packName,
                        "weapons", emptyMap(), weapons)
        }
        if (out.isNotEmpty()) return out   // what else the mod holds is its screenshots

        // Free pictures: a camouflage each, `<x>_swatch` and the like its
        // preview, `<x>_0`, `<x>_1`, ... the frames of one animation.
        class Free(val name: String, val frames: MutableList<Pair<Int, Source>> = mutableListOf())
        val camos = sortedMapOf<String, Free>()
        val previews = HashMap<String, Source>()
        for (file in free) {
            if (file.name.endsWith(".iwi")) continue
            val stem = file.name.substringBeforeLast('.')
            val base = stem.substringAfterLast('/')
            val preview = Regex("^ui_camoskin_(.+?)(?:_sm)?$").matchEntire(base)
                ?: Regex("^(.+)_(?:swatch|icon|preview)$").matchEntire(base)
            if (preview != null) {
                previews[ident(tidy(preview.groupValues[1]) ?: preview.groupValues[1])] = file
                continue
            }
            var name = tidy(stem) ?: continue
            var order = 0
            val folder = stem.substringBeforeLast('/', "").substringAfterLast('/')
            val frame = Regex("^(.+?)[._-](\\d{1,4})$").matchEntire(name)
            if (name.all(Char::isDigit) && folder.isNotEmpty()) {
                order = name.toInt()
                name = tidy(folder) ?: folder
            } else if (frame != null) {
                name = frame.groupValues[1]
                order = frame.groupValues[2].toInt()
            }
            val key = ident(name)
            if (key.isNotEmpty()) camos.getOrPut(key) { Free(title(name)) }.frames += order to file
        }
        for ((key, camo) in camos) {
            val maps = previews[key]?.let { mapOf("swatch" to it) } ?: emptyMap()
            out += Slot(key, names[key] ?: camo.name, "", maps, frames = camo.frames.sortedBy { it.first }.map { it.second })
        }
        return out
    }

    // ---- pictures ----

    /** The picture at another size: each texel the average of the area it covers. */
    fun resize(picture: Picture, toWidth: Int, toHeight: Int): Picture {
        if (picture.width == toWidth && picture.height == toHeight) return picture
        val out = IntArray(toWidth * toHeight)
        for (y in 0 until toHeight) {
            val y0 = (y.toLong() * picture.height / toHeight).toInt()
            val y1 = max(y0 + 1, ((y + 1).toLong() * picture.height / toHeight).toInt())
            for (x in 0 until toWidth) {
                val x0 = (x.toLong() * picture.width / toWidth).toInt()
                val x1 = max(x0 + 1, ((x + 1).toLong() * picture.width / toWidth).toInt())
                var a = 0; var r = 0; var g = 0; var b = 0
                for (v in y0 until y1) for (u in x0 until x1) {
                    val texel = picture.argb[v * picture.width + u]
                    a += texel ushr 24; r += texel shr 16 and 0xFF; g += texel shr 8 and 0xFF; b += texel and 0xFF
                }
                val count = (y1 - y0) * (x1 - x0)
                out[y * toWidth + x] = (a / count shl 24) or (r / count shl 16) or (g / count shl 8) or (b / count)
            }
        }
        return Picture(toWidth, toHeight, out)
    }

    /** No longer than [size] on its longest side. */
    fun fit(picture: Picture, size: Int): Picture {
        val longest = max(picture.width, picture.height)
        if (longest <= size) return picture
        return resize(picture, max(4, picture.width * size / longest), max(4, picture.height * size / longest))
    }

    /** The menu's small preview, made from the camouflage when a mod has none. */
    fun swatchOf(picture: Picture): Picture {
        val side = min(picture.width, picture.height)
        val square = IntArray(side * side) { picture.argb[(it / side) * picture.width + it % side] }
        return resize(Picture(side, side, square), SWATCH, SWATCH)
    }

    /**
     * A camouflage tile out of a whole-weapon skin: the window of the sheet
     * with the most fine detail in every quarter of it -- the painted pattern,
     * not the flat or smooth parts around it -- mirrored into a square that
     * repeats without a seam. The tile, and how much detail it has.
     */
    fun tileOf(sheet: Picture, work: Int = 512, window: Int = 64): Pair<Picture, Float> {
        val longest = max(sheet.width, sheet.height)
        val small = resize(sheet, max(window, sheet.width * work / longest), max(window, sheet.height * work / longest))
        val w = small.width
        val h = small.height
        val grey = FloatArray(w * h) {
            val t = small.argb[it]
            ((t shr 16 and 0xFF) + (t shr 8 and 0xFF) + (t and 0xFF)) / 3f
        }
        // Summed area of the detail (a Laplacian's size), for a window's quarters.
        val total = DoubleArray((w + 1) * (h + 1))
        for (y in 0 until h) {
            var row = 0.0
            for (x in 0 until w) {
                if (x in 1 until w - 1 && y in 1 until h - 1) {
                    val i = y * w + x
                    row += abs(4 * grey[i] - grey[i - 1] - grey[i + 1] - grey[i - w] - grey[i + w])
                }
                total[(y + 1) * (w + 1) + x + 1] = total[y * (w + 1) + x + 1] + row
            }
        }
        fun area(x: Int, y: Int, side: Int) =
            total[(y + side) * (w + 1) + x + side] - total[y * (w + 1) + x + side] - total[(y + side) * (w + 1) + x] + total[y * (w + 1) + x]
        val q = window / 2
        var best = -1.0
        var bestX = 0
        var bestY = 0
        for (y in 0..h - window step 8) for (x in 0..w - window step 8) {
            val score = minOf(area(x, y, q), area(x + q, y, q), area(x, y + q, q), area(x + q, y + q, q))
            if (score > best) { best = score; bestX = x; bestY = y }
        }
        val side = window * 2
        val tile = IntArray(side * side) {
            val x = it % side
            val y = it / side
            val u = if (x < window) x else side - 1 - x
            val v = if (y < window) y else side - 1 - y
            small.argb[(bestY + v) * w + bestX + u] or (0xFF shl 24)
        }
        return Picture(side, side, tile) to best.toFloat()
    }

    // ---- the known packs ----

    /** The entry of setup/camo-packs.json whose `match` the mod's file name fits. */
    fun known(list: String, fileName: String): Known? {
        val base = fileName.substringAfterLast('/').substringBeforeLast('.').lowercase()
        val packs = try { JSONObject(list).getJSONArray("packs") } catch (_: Exception) { return null }
        for (i in 0 until packs.length()) {
            val pack = packs.optJSONObject(i) ?: continue
            val patterns = pack.optJSONArray("match") ?: continue
            val fits = (0 until patterns.length()).any { glob(patterns.optString(it)).matches(base) }
            if (!fits) continue
            val names = HashMap<String, String>()
            pack.optJSONObject("names")?.let { for (key in it.keys()) names[key] = it.optString(key) }
            return Known(pack.optString("id"), pack.optString("name"), pack.optString("label"),
                         pack.optString("author"), pack.optString("url"), names)
        }
        return null
    }

    private fun glob(pattern: String) =
        Regex(pattern.split("*").joinToString(".*") { Regex.escape(it) })

    // ---- writing the pack ----

    /**
     * Writes the mod as the pack folder [out] and returns how many
     * camouflages it has; none, and no folder, when the mod holds no
     * camouflage. [decode] turns a file that is not an IWI into a picture and
     * [save] writes one as a PNG (both Android's, CamoPacksActivity).
     */
    fun write(
        files: List<Source>, out: File, name: String, author: String, url: String, label: String,
        names: Map<String, String>, decode: (ByteArray) -> Picture, save: (Picture, File) -> Unit,
    ): Int {
        fun read(source: Source): Picture =
            if (source.name.endsWith(".iwi")) decodeIwi(source.read()) else decode(source.read())

        val entries = JSONArray()
        for (camo in collect(files, name, names)) {
            try {
                val entry = JSONObject().put("id", camo.id).put("name", camo.name)
                if (camo.target.isNotEmpty()) entry.put("target", camo.target)
                fun put(picture: Picture, relative: String): String {
                    val file = File(out, relative)
                    file.parentFile?.mkdirs()
                    save(picture, file)
                    return relative
                }
                var drawn: Picture? = null
                if (camo.weapons.isNotEmpty()) {
                    val weapons = JSONObject()
                    var best = -1f
                    for ((weapon, source) in camo.weapons) {
                        val (tile, detail) = try { tileOf(read(source)) } catch (_: Exception) { continue }
                        weapons.put(weapon, put(tile, "${camo.id}/$weapon.png"))
                        if (detail > best) { best = detail; drawn = tile }
                    }
                    val richest = drawn ?: throw Refused("none of its weapon skins decodes")
                    entry.put("weapons", weapons).put("image", put(richest, "${camo.id}.png"))
                } else if (camo.frames.isNotEmpty()) {
                    val frames = camo.frames.map { fit(read(it), SIZE) }.let { all ->
                        all.filter { it.width == all[0].width && it.height == all[0].height }
                    }
                    drawn = frames[0]
                    if (frames.size > 1) {
                        entry.put("frames", JSONArray(frames.mapIndexed { n, frame -> put(frame, "${camo.id}/$n.png") }))
                        entry.put("fps", 12)
                    } else {
                        entry.put("image", put(frames[0], "${camo.id}.png"))
                    }
                } else {
                    // Gold's image on the console is its specular map, so a
                    // mod's specular map is what is drawn; its colour map is
                    // kept beside it.
                    for ((role, suffix) in listOf("specular" to "spc", "colour" to "col")) {
                        val picture = fit(read(camo.maps[role] ?: continue), SIZE)
                        entry.put(role, put(picture, "${camo.id}_$suffix.png"))
                        if (drawn == null) drawn = picture
                    }
                }
                val picture = drawn ?: continue
                val small = try {
                    camo.maps["swatch"]?.let { fit(read(it), SWATCH) }
                } catch (_: Exception) {
                    null
                } ?: swatchOf(picture)
                entry.put("swatch", put(small, "${camo.id}_swatch.png"))
                camo.maps["banner"]?.let {
                    try {
                        entry.put("banner", put(resize(read(it), BANNER_WIDTH, BANNER_HEIGHT), "${camo.id}_banner.png"))
                    } catch (_: Exception) {
                        // The camouflage itself is the banner then.
                    }
                }
                entries.put(entry)
            } catch (_: Exception) {
                // A camouflage whose pictures do not decode is left out.
            }
        }
        if (entries.length() == 0) {
            out.deleteRecursively()
            return 0
        }
        val pack = JSONObject().put("name", name).put("author", author).put("url", url)
        if (label.isNotEmpty()) pack.put("label", label)
        pack.put("camos", entries)
        // Last, as the pack's mark of being whole (CamoPacks.installBundled).
        File(out, "pack.json").writeText(pack.toString(2) + "\n")
        return entries.length()
    }
}
