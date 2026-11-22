package com.mw2.recomp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * CamoImport's rules, on mods made here: what each file of a PC camouflage
 * mod is, which camouflages come of them and under what names, and that an
 * IWI's picture is read texel for texel. The same cases as
 * `tools/import_pack.py --self-test`, since the two must make the same pack.
 */
class CamoImportTest {
    private fun iwi(width: Int, height: Int, seed: Int): ByteArray {
        // Version 8, format 1 (BGRA), no mip chain: a header and the picture.
        val out = ByteArray(32 + width * height * 4)
        "IWi".toByteArray().copyInto(out)
        out[3] = 8
        out[4] = 2
        out[8] = 1
        out[10] = (width and 0xFF).toByte(); out[11] = (width shr 8).toByte()
        out[12] = (height and 0xFF).toByte(); out[13] = (height shr 8).toByte()
        for (i in 32 until out.size) out[i] = ((i * 31 + seed * 17) xor (i shr 3)).toByte()
        return out
    }

    private fun source(name: String, data: ByteArray) = CamoImport.Source(name) { data }

    @Test
    fun anIwiIsReadTexelForTexel() {
        val data = iwi(8, 4, 1)
        val picture = CamoImport.decodeIwi(data)
        assertEquals(8, picture.width)
        assertEquals(4, picture.height)
        fun u8(at: Int) = data[at].toInt() and 0xFF
        val first = (u8(35) shl 24) or (u8(34) shl 16) or (u8(33) shl 8) or u8(32)
        assertEquals(first, picture.argb[0])
    }

    @Test
    fun aDxt1BlockOfOneColour() {
        val data = ByteArray(32 + 8)
        "IWi".toByteArray().copyInto(data)
        data[3] = 8; data[4] = 2; data[8] = 0xB; data[10] = 4; data[12] = 4
        // Both end colours pure red in 5:6:5, every texel the first.
        data[32] = 0x00; data[33] = 0xF8.toByte(); data[34] = 0x00; data[35] = 0xF8.toByte()
        val picture = CamoImport.decodeIwi(data)
        assertArrayEquals(IntArray(16) { 0xFFFF0000.toInt() }, picture.argb)
    }

    @Test
    fun whatAFileIs() {
        assertEquals(Triple("camo", "gold", "colour"), CamoImport.classify("detail_gold_col"))
        assertEquals(Triple("camo", "gold", "specular"), CamoImport.classify("~detail_gold_spc-r-20g-20b-20~cf35b1a3"))
        assertEquals(Triple("camo", "gold", "banner"), CamoImport.classify("weapon_camo_menu_gold"))
        assertEquals(Triple("camo", "d_urban", "swatch"), CamoImport.classify("weapon_camo_menu_d_urban_sm"))
        assertEquals(Triple("camo", "red", "colour"), CamoImport.classify("weapon_camo_red"))
        assertEquals(Triple("skin", "iw5_ak47", "colour"), CamoImport.classify("weapon_ak47_iw5_col"))
        assertEquals(Triple("skin", "iw5_msr", "specular"), CamoImport.classify("~weapon_remington_msr_spc-rgb~655e7a06"))
        assertEquals(Triple("skin", "iw5_m9", "colour"), CamoImport.classify("uzi_m9_col"))
        assertEquals("skip", CamoImport.classify("weapon_ak47_iw5_nml")?.third)
        assertEquals("skip", CamoImport.classify("weapon_as50_scope_col")?.third)
        assertNull(CamoImport.classify("screenshot"))
    }

    @Test
    fun slothsLayoutListsEachCamoOnceUnderItsFolderName() {
        val red = iwi(8, 8, 1)
        val files = listOf(
            source("x/all/weapon_camo_red.iwi", red),
            source("x/red - crimson/weapon_camo_red.iwi", red),
            source("x/red - crimson/weapon_camo_menu_red.iwi", iwi(8, 8, 2)),
            source("x/all/weapon_camo_hex.iwi", iwi(8, 8, 3)),
            source("x/all/detail_gold_col.iwi", iwi(4, 4, 4)),
            source("x/all/~detail_gold_spc-r-20g-20b-20~cf35b1a3.iwi", iwi(8, 8, 5)),
            source("x/gold - rainbow/detail_gold_col.iwi", iwi(4, 4, 4)),
            source("x/gold - rainbow/~detail_gold_spc-r-20g-20b-20~cf35b1a3.iwi", iwi(8, 8, 5)),
            source("screenshot.png", ByteArray(4)),
        )
        val slots = CamoImport.collect(files, "Colours", mapOf("hex" to "Lime")).associateBy { it.id }
        assertEquals(setOf("red", "hex", "gold"), slots.keys)
        assertEquals("Crimson", slots.getValue("red").name)
        assertEquals("Lime", slots.getValue("hex").name)
        assertEquals("Rainbow", slots.getValue("gold").name)
        assertEquals("gold", slots.getValue("gold").target)
        assertEquals("weapon_camo_red", slots.getValue("red").target)
        assertTrue("banner" in slots.getValue("red").maps && "specular" in slots.getValue("gold").maps)
    }

    @Test
    fun aModOfGoldAloneIsOneCamoNamedAfterTheMod() {
        val slots = CamoImport.collect(
            listOf(source("nebula/iw5/images/detail_gold_col.iwi", iwi(4, 4, 1))), "Nebula", emptyMap(),
        )
        assertEquals(listOf("gold" to "Nebula"), slots.map { it.id to it.name })
    }

    @Test
    fun weaponSkinsAreOneCamoWithATileAWeapon() {
        val slots = CamoImport.collect(
            listOf(
                source("jewel/ak-47/iw_06/weapon_ak47_iw5_col.iwi", iwi(64, 64, 1)),
                source("jewel/ak-47/iw_06/weapon_ak47_iw5_nml.iwi", iwi(8, 8, 2)),
                source("jewel/msr/iw_06/weapon_remington_msr_col.iwi", iwi(64, 64, 3)),
                source("jewel/msr/iw_06/weapon_remington_msr_scope_col.iwi", iwi(8, 8, 4)),
                source("jewel/ak-47/ak-47.png", ByteArray(4)),
            ),
            "Jewel", emptyMap(),
        )
        assertEquals(1, slots.size)
        assertEquals("Jewel", slots[0].name)
        assertEquals(setOf("iw5_ak47", "iw5_msr"), slots[0].weapons.keys)
        val (tile, _) = CamoImport.tileOf(CamoImport.decodeIwi(iwi(64, 64, 1)))
        assertEquals(128, tile.width)
        // Mirrored both ways, so it repeats without a seam.
        assertEquals(tile.argb[5], tile.argb[127 - 5])
        assertEquals(tile.argb[5], tile.argb[127 * 128 + 5])
    }

    @Test
    fun freePicturesAreACamoEachWithPreviewsAndFrames() {
        val slots = CamoImport.collect(
            listOf(
                source("images/tiger stripe.png", ByteArray(1)),
                source("images/tiger_stripe_n.png", ByteArray(1)),
                source("images/ui_camoskin_tiger_stripe.png", ByteArray(1)),
                source("images/lava/0.png", ByteArray(1)),
                source("images/lava/1.png", ByteArray(1)),
            ),
            "Free", emptyMap(),
        ).associateBy { it.id }
        assertEquals(setOf("tiger-stripe", "lava"), slots.keys)
        assertEquals(2, slots.getValue("lava").frames.size)
        assertTrue("swatch" in slots.getValue("tiger-stripe").maps)
    }
}
