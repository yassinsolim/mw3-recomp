package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.InputType
import android.text.method.LinkMovementMethod
import android.text.util.Linkify
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.Switch
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream
import kotlin.math.roundToInt

/**
 * The camouflage packs of Modern Warfare 3's multiplayer: which are
 * installed, whose each is and where its author published it, what its
 * camouflages look like, and which of them the game lists. A pack is added
 * from a PC camouflage mod the player downloaded (a zip, or its loose .iwi
 * pictures), converted here (CamoImport), or fetched from the project's
 * release when its art is its author's own to pass on.
 */
class CamoPacksActivity : AppCompatActivity() {

    private lateinit var gameDirectory: String
    private lateinit var list: ListView
    private lateinit var header: View
    private var packs: List<CamoPacks.Pack> = emptyList()
    private var options = CamoPacks.Options()

    private val pickFiles = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val data = result.data ?: return@registerForActivityResult
        val clip = data.clipData
        val uris = if (clip != null) (0 until clip.itemCount).map { clip.getItemAt(it).uri } else listOfNotNull(data.data)
        if (uris.isNotEmpty()) add(uris)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_camo_packs)
        title = getString(R.string.camo_packs)
        gameDirectory = Prefs(this).gameDirectory.ifEmpty {
            (getExternalFilesDir(null) ?: filesDir).absolutePath + "/game"
        }
        list = findViewById(R.id.camo_pack_list)
        header = layoutInflater.inflate(R.layout.camo_packs_header, list, false)
        list.addHeaderView(header, null, false)

        header.findViewById<Button>(R.id.camo_pack_add).setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            intent.addCategory(Intent.CATEGORY_OPENABLE)
            // File providers label an .iwi, and often a zip, as octet-stream.
            intent.type = "*/*"
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            pickFiles.launch(intent)
        }
        val fetch: Button = header.findViewById(R.id.camo_pack_fetch)
        fetch.visibility = if (hosted().isEmpty()) View.GONE else View.VISIBLE
        fetch.setOnClickListener { fetch() }
        val animateGold: Switch = header.findViewById(R.id.camo_animate_gold)
        options = CamoPacks.options(gameDirectory)
        animateGold.isChecked = options.animateGold
        animateGold.setOnCheckedChangeListener { _, on ->
            options.animateGold = on
            CamoPacks.save(gameDirectory, options)
        }
        refresh()
    }

    private fun refresh() {
        packs = CamoPacks.list(this, gameDirectory)
        options = CamoPacks.options(gameDirectory)
        header.findViewById<TextView>(R.id.camo_pack_count).text =
            if (packs.isEmpty()) getString(R.string.camo_packs_none)
            else resources.getQuantityString(
                R.plurals.camo_pack_count, packs.size, packs.size,
                packs.filter { it.id !in options.off }.sumOf { it.camos.size },
            )
        // The same place in the list after a switch is flipped.
        val at = list.firstVisiblePosition
        val offset = list.getChildAt(0)?.top ?: 0
        list.adapter = Adapter()
        list.setSelectionFromTop(at, offset - list.paddingTop)
    }

    // ---- adding a mod the player picked ----

    private fun nameOf(uri: Uri): String {
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) return it.getString(0) ?: ""
            }
        } catch (_: Throwable) {
            // The path's last part will do.
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: ""
    }

    private fun add(uris: List<Uri>) {
        val names = uris.map { nameOf(it) }
        val archives = names.indices.filter { names[it].lowercase().let { n -> n.endsWith(".zip") || n.endsWith(".iwd") } }
        val unread = names.filter { it.lowercase().let { n -> n.endsWith(".rar") || n.endsWith(".7z") } }
        if (unread.isNotEmpty()) {
            say(getString(R.string.camo_pack_add), getString(R.string.camo_pack_rar, unread.joinToString(", ")))
            return
        }
        val loose = names.indices.filter { it !in archives && CamoImport.isPicture(names[it]) }
        if (archives.isEmpty() && loose.isEmpty()) {
            say(getString(R.string.camo_pack_add), getString(R.string.camo_pack_no_pictures))
            return
        }
        // Loose pictures have no file that names the pack: the player does.
        if (loose.isNotEmpty()) {
            val input = EditText(this).apply {
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
                setText(R.string.camo_pack_default_name)
                selectAll()
            }
            AlertDialog.Builder(this)
                .setTitle(R.string.camo_pack_name)
                .setView(input)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.done) { _, _ ->
                    convert(uris, names, archives, loose, input.text.toString().trim().ifEmpty { getString(R.string.camo_pack_default_name) })
                }
                .show()
        } else {
            convert(uris, names, archives, loose, "")
        }
    }

    private fun convert(uris: List<Uri>, names: List<String>, archives: List<Int>, loose: List<Int>, looseName: String) {
        val progress = AlertDialog.Builder(this)
            .setTitle(R.string.camo_pack_add)
            .setMessage(R.string.camo_pack_converting)
            .setCancelable(false)
            .show()
        Thread {
            val known = try {
                assets.open(CamoPacks.KNOWN_ASSET).use { it.readBytes().decodeToString() }
            } catch (_: Throwable) {
                ""
            }
            val report = StringBuilder()
            for (index in archives) {
                val copy = File(cacheDir, "camo-import.zip")
                try {
                    contentResolver.openInputStream(uris[index])!!.use { input -> copy.outputStream().use { input.copyTo(it) } }
                    ZipFile(copy).use { zip -> report.appendLine(write(sources(zip), names[index], known, "")) }
                } catch (e: Throwable) {
                    report.appendLine(getString(R.string.camo_pack_failed, names[index], e.message ?: e.javaClass.simpleName))
                } finally {
                    copy.delete()
                }
            }
            if (loose.isNotEmpty()) {
                val files = loose.map { index ->
                    CamoImport.Source(names[index].lowercase()) { contentResolver.openInputStream(uris[index])!!.use { it.readBytes() } }
                }
                report.appendLine(try {
                    write(files, looseName, known, looseName)
                } catch (e: Throwable) {
                    getString(R.string.camo_pack_failed, looseName, e.message ?: e.javaClass.simpleName)
                })
            }
            runOnUiThread {
                progress.dismiss()
                refresh()
                say(getString(R.string.camo_pack_add), report.toString().trim())
            }
        }.start()
    }

    /** The pictures of a zip, and of the .iwd and .zip files inside it. */
    private fun sources(zip: ZipFile): List<CamoImport.Source> {
        val files = mutableListOf<CamoImport.Source>()
        for (entry in zip.entries()) {
            if (entry.isDirectory) continue
            val name = entry.name.replace('\\', '/').lowercase()
            if (name.endsWith(".iwd") || name.endsWith(".zip")) {
                ZipInputStream(zip.getInputStream(entry)).use { inner ->
                    while (true) {
                        val member = inner.nextEntry ?: break
                        val inside = "$name/" + member.name.replace('\\', '/').lowercase()
                        if (member.isDirectory || !CamoImport.isPicture(inside)) continue
                        val bytes = inner.readBytes()
                        files += CamoImport.Source(inside) { bytes }
                    }
                }
            } else if (CamoImport.isPicture(name)) {
                files += CamoImport.Source(name) { zip.getInputStream(entry).use { it.readBytes() } }
            }
        }
        return files
    }

    /** Converts one mod into its pack folder; the line that says what came of it. */
    private fun write(files: List<CamoImport.Source>, fileName: String, knownList: String, givenName: String): String {
        val known = CamoImport.known(knownList, fileName)
        val name = givenName.ifEmpty { known?.name ?: CamoImport.title(fileName.substringBeforeLast('.')) }
        val id = known?.id?.ifEmpty { null } ?: CamoImport.ident(name).ifEmpty { "pack" }
        val out = File(CamoPacks.folder(gameDirectory), id)
        out.deleteRecursively()
        val count = CamoImport.write(
            files, out, name, known?.author ?: "", known?.url ?: "", known?.label ?: "", known?.names ?: emptyMap(),
            decode = { bytes ->
                val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: throw CamoImport.Refused("not a picture")
                val argb = IntArray(bitmap.width * bitmap.height)
                bitmap.getPixels(argb, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
                CamoImport.Picture(bitmap.width, bitmap.height, argb).also { bitmap.recycle() }
            },
            save = { picture, file ->
                val bitmap = Bitmap.createBitmap(picture.argb, picture.width, picture.height, Bitmap.Config.ARGB_8888)
                file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
                bitmap.recycle()
            },
        )
        return if (count == 0) getString(R.string.camo_pack_nothing, fileName)
        else resources.getQuantityString(R.plurals.camo_pack_added, count, name, count)
    }

    // ---- the project's own release ----

    private class Hosted(val id: String, val name: String, val author: String, val url: String, val sha256: String)

    /** The packs of the bundled list that the project's release holds. */
    private fun hosted(): List<Hosted> = try {
        val list = JSONObject(assets.open(CamoPacks.KNOWN_ASSET).use { it.readBytes().decodeToString() })
        val release = list.getJSONObject("release")
        val base = "https://github.com/${release.getString("repo")}/releases/download/${release.getString("tag")}/"
        val packs = list.getJSONArray("packs")
        (0 until packs.length()).map { packs.getJSONObject(it) }.filter { it.optString("asset").isNotEmpty() }.map {
            Hosted(it.getString("id"), it.getString("name"), it.optString("author"), base + it.getString("asset"), it.optString("sha256"))
        }
    } catch (_: Throwable) {
        emptyList()
    }

    private fun fetch() {
        val progress = AlertDialog.Builder(this)
            .setTitle(R.string.camo_pack_fetch)
            .setMessage(R.string.camo_pack_fetching)
            .setCancelable(false)
            .show()
        Thread {
            val report = StringBuilder()
            for (pack in hosted()) {
                val archive = File(cacheDir, "camo-fetch.zip")
                try {
                    val connection = URL(pack.url).openConnection() as HttpURLConnection
                    connection.connectTimeout = 15000
                    connection.readTimeout = 30000
                    if (connection.responseCode != 200) throw IllegalStateException("HTTP ${connection.responseCode}")
                    val digest = MessageDigest.getInstance("SHA-256")
                    connection.inputStream.use { input ->
                        archive.outputStream().use { output ->
                            val buffer = ByteArray(1 shl 16)
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                digest.update(buffer, 0, read)
                                output.write(buffer, 0, read)
                            }
                        }
                    }
                    if (digest.digest().joinToString("") { "%02x".format(it) } != pack.sha256.lowercase()) {
                        throw IllegalStateException(getString(R.string.camo_pack_checksum))
                    }
                    // The archive is the pack's folder, `<id>/...`; nothing of it may land outside.
                    val root = CamoPacks.folder(gameDirectory)
                    CamoPacks.remove(gameDirectory, pack.id)
                    ZipFile(archive).use { zip ->
                        for (entry in zip.entries().toList().sortedBy { it.name.endsWith("pack.json") }) {
                            val target = File(root, entry.name)
                            if (entry.isDirectory || !target.canonicalPath.startsWith(File(root, pack.id).canonicalPath + File.separator)) continue
                            target.parentFile?.mkdirs()
                            zip.getInputStream(entry).use { input -> target.outputStream().use { input.copyTo(it) } }
                        }
                    }
                    report.appendLine(getString(R.string.camo_pack_fetched, pack.name, pack.author))
                } catch (e: Throwable) {
                    report.appendLine(getString(R.string.camo_pack_failed, pack.name, e.message ?: e.javaClass.simpleName))
                } finally {
                    archive.delete()
                }
            }
            runOnUiThread {
                progress.dismiss()
                refresh()
                say(getString(R.string.camo_pack_fetch), report.toString().trim())
            }
        }.start()
    }

    private fun say(title: String, text: String) {
        AlertDialog.Builder(this).setTitle(title).setMessage(text).setPositiveButton(R.string.close, null).show()
    }

    private fun confirmRemove(pack: CamoPacks.Pack) {
        // A pack this app carries is copied back at the next start: it is switched off instead.
        if (pack.bundled) {
            say(pack.name, getString(R.string.camo_pack_bundled))
            return
        }
        AlertDialog.Builder(this)
            .setTitle(pack.name)
            .setMessage(R.string.camo_pack_remove_hint)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.camo_pack_remove) { _, _ ->
                CamoPacks.remove(gameDirectory, pack.id)
                options.off -= pack.id
                options.animate -= pack.id
                CamoPacks.save(gameDirectory, options)
                refresh()
            }
            .show()
    }

    /** A pack a row: its name and switch, whose it is, its link, its camouflages and their previews. */
    private inner class Adapter : BaseAdapter() {
        override fun getCount(): Int = packs.size
        override fun getItem(position: Int): Any = packs[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val pack = packs[position]
            val density = resources.displayMetrics.density
            fun dp(value: Int) = (value * density).roundToInt()
            // Built anew each time: a row's switches carry listeners of the pack they were made for.
            val row = LinearLayout(this@CamoPacksActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), dp(12), dp(12), dp(12))
                setBackgroundResource(R.drawable.bg_card)
            }
            val on = pack.id !in options.off
            val head = LinearLayout(this@CamoPacksActivity).apply { gravity = Gravity.CENTER_VERTICAL }
            head.addView(TextView(this@CamoPacksActivity).apply {
                text = pack.name
                textSize = 16f
                setTextColor(0xFFFFFFFF.toInt())
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            head.addView(CheckBox(this@CamoPacksActivity).apply {
                text = getString(R.string.camo_pack_animate)
                setTextColor(0xFFC8D0D8.toInt())
                isChecked = pack.id in options.animate
                setOnCheckedChangeListener { _, checked ->
                    if (checked) options.animate += pack.id else options.animate -= pack.id
                    CamoPacks.save(gameDirectory, options)
                }
            })
            head.addView(Switch(this@CamoPacksActivity).apply {
                isChecked = on
                contentDescription = getString(R.string.camo_pack_listed)
                setPadding(dp(12), 0, 0, 0)
                setOnCheckedChangeListener { _, checked ->
                    if (checked) options.off -= pack.id else options.off += pack.id
                    CamoPacks.save(gameDirectory, options)
                    refresh()
                }
            })
            row.addView(head)
            row.addView(TextView(this@CamoPacksActivity).apply {
                text = resources.getQuantityString(
                    R.plurals.camo_pack_by, pack.camos.size, pack.camos.size,
                    pack.author.ifEmpty { getString(R.string.camo_pack_unnamed) },
                ) + if (pack.url.isEmpty()) "" else "\n" + pack.url
                textSize = 12f
                setTextColor(0xFFC8D0D8.toInt())
                // The author's link opens where they published the pack.
                Linkify.addLinks(this, Linkify.WEB_URLS)
                movementMethod = LinkMovementMethod.getInstance()
            })
            row.addView(TextView(this@CamoPacksActivity).apply {
                text = pack.camos.joinToString(" · ")
                textSize = 11f
                setTextColor(0xFF8A94A0.toInt())
                maxLines = 2
            })
            if (pack.previews.isNotEmpty()) {
                val strip = LinearLayout(this@CamoPacksActivity)
                for (file in pack.previews.take(PREVIEWS)) {
                    val picture = BitmapFactory.decodeFile(file.path) ?: continue
                    strip.addView(ImageView(this@CamoPacksActivity).apply {
                        setImageBitmap(picture)
                        scaleType = ImageView.ScaleType.CENTER_CROP
                    }, LinearLayout.LayoutParams(dp(40), dp(40)).apply { setMargins(0, dp(8), dp(6), 0) })
                }
                row.addView(strip)
            }
            row.alpha = if (on) 1f else 0.55f
            row.setOnLongClickListener {
                confirmRemove(pack)
                true
            }
            return row
        }
    }

    private companion object {
        // As many previews as a row is wide on a phone held sideways.
        const val PREVIEWS = 14
    }
}
