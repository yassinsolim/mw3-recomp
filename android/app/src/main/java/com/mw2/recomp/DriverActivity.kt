package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.DocumentsContract
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.IOException
import java.util.Locale
import kotlin.concurrent.thread
import kotlin.math.roundToInt

/**
 * Choosing which Vulkan the game talks to: the one the phone shipped with,
 * a driver the catalog knows for this chip (DriverCatalog, fetched from its
 * publisher and checked against the catalog's SHA-256), or one the player
 * brought: a zip picked from a file, or a link, typed here or shared to the
 * app from a browser or a chat.
 *
 * Whatever its source, a driver is tested before it is used
 * (DriverSelfTest): it has to draw the test frame right on this phone. One
 * the catalog does not know is native code nobody vouches for, and the
 * player is told so and shown its SHA-256 before it is unpacked.
 */
class DriverActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs
    private lateinit var store: DriverStore
    private lateinit var current: TextView
    private lateinit var list: ListView
    private lateinit var catalog: DriverCatalog
    private var drivers: List<DriverStore.Driver> = emptyList()
    private var offered: List<DriverCatalog.Entry> = emptyList()
    private var busy: AlertDialog? = null

    private val pickZip = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val uri = result.data?.data ?: return@registerForActivityResult
        importFrom(uri.lastPathSegment ?: "driver") { target ->
            (contentResolver.openInputStream(uri) ?: throw IOException(getString(R.string.driver_bad_zip))).use { input ->
                target.outputStream().use { input.copyTo(it, 1 shl 16) }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_driver)
        prefs = Prefs(this)
        store = DriverStore(this)
        catalog = DriverAuto.catalog(this)
        title = getString(R.string.graphics_driver)

        current = findViewById(R.id.driver_current)
        list = findViewById(R.id.driver_list)

        findViewById<Button>(R.id.import_driver).setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            intent.addCategory(Intent.CATEGORY_OPENABLE)
            // Some file providers label a zip as octet-stream, so everything
            // is offered rather than the player being told there is nothing
            // to pick.
            intent.type = "*/*"
            intent.putExtra(
                Intent.EXTRA_MIME_TYPES,
                arrayOf("application/zip", "application/octet-stream", "*/*"),
            )
            // Where a browser left it.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                intent.putExtra(
                    DocumentsContract.EXTRA_INITIAL_URI,
                    Uri.parse("content://com.android.externalstorage.documents/document/primary%3ADownload"),
                )
            }
            pickZip.launch(intent)
        }
        findViewById<Button>(R.id.import_driver_link).setOnClickListener { askForLink("") }

        list.setOnItemClickListener { _, _, position, _ ->
            when {
                position == 0 -> chooseSystem()
                position <= drivers.size -> testThenChoose(drivers[position - 1])
                else -> confirmFetch(offered[position - 1 - drivers.size])
            }
        }
        list.setOnItemLongClickListener { _, _, position, _ ->
            if (position in 1..drivers.size) confirmRemove(drivers[position - 1])
            true
        }
        refresh()
        if (savedInstanceState == null) handleShared(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleShared(intent)
    }

    override fun onDestroy() {
        busy?.dismiss()
        busy = null
        super.onDestroy()
    }

    /**
     * What another app handed over: a zip (a file manager's Share or Open
     * with), or text with a link in it (a browser's or a chat's Share).
     * Nothing is done with either before the player has said so here.
     */
    private fun handleShared(intent: Intent?) {
        intent ?: return
        val stream: Uri? = when (intent.action) {
            Intent.ACTION_SEND -> if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM)
            Intent.ACTION_VIEW -> intent.data?.takeIf { it.scheme == "content" }
            else -> return
        }
        if (stream != null) {
            importFrom(stream.lastPathSegment ?: "driver") { target ->
                (contentResolver.openInputStream(stream) ?: throw IOException(getString(R.string.driver_bad_zip))).use { input ->
                    target.outputStream().use { input.copyTo(it, 1 shl 16) }
                }
            }
            return
        }
        val text = intent.getStringExtra(Intent.EXTRA_TEXT) ?: intent.dataString ?: return
        askForLink(DriverFetch.linkIn(text).orEmpty())
    }

    private fun askForLink(prefilled: String) {
        val field = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            hint = getString(R.string.driver_link_hint)
            setText(prefilled)
        }
        val margin = (20 * resources.displayMetrics.density).roundToInt()
        val holder = LinearLayout(this).apply {
            setPadding(margin, margin / 2, margin, 0)
            addView(field, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.import_driver_link)
            .setMessage(R.string.driver_link_about)
            .setView(holder)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.driver_fetch) { _, _ ->
                val link = DriverFetch.linkIn(field.text.toString().trim())
                if (link == null) say(getString(R.string.driver_link_bad))
                else importFrom(Uri.parse(link).lastPathSegment ?: "driver") { target -> DriverFetch.download(link, target) }
            }
            .show()
    }

    /**
     * A zip from anywhere: [fetch] writes it to the file it is given, and
     * then it is what the catalog says of its SHA-256, or a driver nobody
     * vouches for, which the player is asked about first.
     */
    private fun importFrom(name: String, fetch: (File) -> Unit) {
        val file = File(cacheDir, "driver-incoming.zip")
        work(getString(R.string.driver_fetching), {
            fetch(file)
            DriverStore.sha256(file.inputStream()) to file.length()
        }) { result, error ->
            if (result == null) {
                file.delete()
                say(getString(R.string.driver_fetch_failed, error))
                return@work
            }
            val (sha, size) = result
            val known = catalog.bySha(sha)
            if (known != null) {
                unpackTestChoose(file, known.name, known.id)
                return@work
            }
            AlertDialog.Builder(this)
                .setTitle(R.string.driver_unknown_title)
                .setMessage(getString(R.string.driver_unknown_body, name, megabytes(size), sha))
                .setCancelable(false)
                .setNegativeButton(R.string.cancel) { _, _ -> file.delete() }
                .setPositiveButton(R.string.driver_test_and_use) { _, _ ->
                    DriverAuto.log(this, "the player imports a driver the catalog does not know: $name, SHA-256 $sha")
                    unpackTestChoose(file, name, null)
                }
                .show()
        }
    }

    private fun unpackTestChoose(file: File, name: String, catalogId: String?) {
        val outcome = try {
            store.import(file.inputStream(), name, catalogId)
        } finally {
            file.delete()
        }
        when (outcome) {
            is DriverStore.Outcome.Imported -> {
                refresh()
                testThenChoose(outcome.driver, fresh = true)
            }
            is DriverStore.Outcome.Failed -> say(outcome.reason)
        }
    }

    /** The catalog's entry: who publishes it, under what terms, how large, and then fetched and checked. */
    private fun confirmFetch(entry: DriverCatalog.Entry) {
        // The catalog's own words, in English as it is written: who publishes it and under what terms.
        val message = buildString {
            append(entry.version).append('\n')
            append(entry.source).append('\n')
            append(entry.licence)
            if (entry.note.isNotEmpty()) append("\n\n").append(entry.note)
        }
        AlertDialog.Builder(this)
            .setTitle(entry.name)
            .setMessage(message)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(getString(R.string.driver_get, megabytes(entry.size))) { _, _ ->
                work(getString(R.string.driver_fetching), { DriverAuto.install(this, prefs, entry) }) { driver, error ->
                    refresh()
                    if (driver == null) say(getString(R.string.driver_fetch_failed, error)) else testThenChoose(driver, fresh = true)
                }
            }
            .show()
    }

    /**
     * The self-test, and the driver chosen when it passes. One that fails is
     * not chosen; the player may still say so, having read why.
     */
    private fun testThenChoose(driver: DriverStore.Driver, fresh: Boolean = false) {
        val id = store.idOf(driver, prefs, catalog)
        val known = prefs.driverVerdicts[id]
        if (!fresh && (known == "ok" || known == "good")) {
            choose(driver)
            return
        }
        work(getString(R.string.driver_testing, driver.name), { DriverAuto.test(this, prefs, driver, id) }) { result, error ->
            refresh()
            when {
                result == null -> say(error)
                result.passed -> {
                    choose(driver)
                    Toast.makeText(this, getString(R.string.driver_test_passed, driver.name), Toast.LENGTH_LONG).show()
                }
                else -> AlertDialog.Builder(this)
                    .setTitle(driver.name)
                    .setMessage(getString(R.string.driver_test_failed, result.reason))
                    .setPositiveButton(R.string.close, null)
                    .setNeutralButton(R.string.driver_use_anyway) { _, _ -> choose(driver) }
                    .show()
            }
        }
    }

    /** [task] off the main thread under a message that cannot be dismissed; [then] hears its result, or null and why. */
    private fun <T> work(message: String, task: () -> T, then: (T?, String) -> Unit) {
        busy?.dismiss()
        val dialog = AlertDialog.Builder(this).setMessage(message).setCancelable(false).show()
        busy = dialog
        thread(name = "driver-work") {
            var result: T? = null
            var error = ""
            try {
                result = task()
            } catch (failure: Throwable) {
                error = failure.message ?: failure.toString()
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                dialog.dismiss()
                if (busy === dialog) busy = null
                then(result, error)
            }
        }
    }

    private fun say(text: String) {
        AlertDialog.Builder(this)
            .setTitle(R.string.graphics_driver)
            .setMessage(text)
            .setPositiveButton(R.string.close, null)
            .show()
    }

    private fun megabytes(bytes: Long): String = String.format(Locale.US, "%.1f", bytes / (1024.0 * 1024.0))

    private fun refresh() {
        drivers = store.list()
        // What the catalog has for this chip that is not on the phone yet, as it ranks them.
        val text = DriverAuto.gpuText(this)
        val gpu = Gpu.parse(text)
        offered = catalog.drivers
            .filter { it.fits(gpu, Build.VERSION.SDK_INT) && it.url.isNotEmpty() && store.ofCatalog(it, prefs) == null }
            .sortedByDescending { it.rankOn(gpu) }
        current.text = getString(R.string.driver_in_use, chosenName())
        list.adapter = Adapter()
    }

    private fun chosenName(): String {
        val directory = prefs.driverDirectory
        if (directory.isEmpty()) return getString(R.string.driver_system)
        return drivers.firstOrNull { it.directory.absolutePath == directory }?.name
            ?: getString(R.string.driver_system)
    }

    private fun chooseSystem() {
        // Picking the system's driver is a decision too: the app's own
        // choice must not come back over it.
        prefs.driverChoiceMade = true
        prefs.driverDirectory = ""
        prefs.driverLibrary = ""
        if (NativeBridge.isLoaded()) NativeBridge.nativeSelectDriver("", "")
        refresh()
    }

    private fun choose(driver: DriverStore.Driver) {
        prefs.driverChoiceMade = true
        prefs.driverDirectory = driver.directory.absolutePath
        prefs.driverLibrary = driver.library
        if (NativeBridge.isLoaded()) {
            NativeBridge.nativeSelectDriver(driver.directory.absolutePath, driver.library)
        }
        refresh()
    }

    private fun confirmRemove(driver: DriverStore.Driver) {
        AlertDialog.Builder(this)
            .setTitle(driver.name)
            .setMessage(driver.description)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.driver_remove) { _, _ ->
                if (prefs.driverDirectory == driver.directory.absolutePath) chooseSystem()
                store.remove(driver)
                Toast.makeText(this, R.string.driver_removed, Toast.LENGTH_SHORT).show()
                refresh()
            }
            .show()
    }

    /** What this phone found out about a driver, in the player's language, or "". */
    private fun verdictText(id: String): String {
        val verdict = prefs.driverVerdicts[id] ?: return ""
        return when {
            verdict == "good" -> getString(R.string.driver_status_good)
            verdict == "ok" -> getString(R.string.driver_status_ok)
            verdict.startsWith("fail") -> getString(R.string.driver_status_failed, verdict.removePrefix("fail:").trim())
            else -> ""
        }
    }

    /** The system's driver, every unpacked one, then what the catalog has for this chip. */
    private inner class Adapter : BaseAdapter() {
        override fun getCount(): Int = 1 + drivers.size + offered.size
        override fun getItem(position: Int): Any = when {
            position == 0 -> "system"
            position <= drivers.size -> drivers[position - 1]
            else -> offered[position - 1 - drivers.size]
        }

        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = (convertView as? LinearLayout) ?: LinearLayout(this@DriverActivity).apply {
                orientation = LinearLayout.VERTICAL
                val margin = (12 * resources.displayMetrics.density).roundToInt()
                setPadding(margin, margin, margin, margin)
                addView(TextView(this@DriverActivity).apply { id = android.R.id.text1; textSize = 15f })
                addView(TextView(this@DriverActivity).apply { id = android.R.id.text2; textSize = 11f })
            }
            val title = row.findViewById<TextView>(android.R.id.text1)
            val detail = row.findViewById<TextView>(android.R.id.text2)

            val chosenDirectory = prefs.driverDirectory
            var chosen = false
            when {
                position == 0 -> {
                    title.text = getString(R.string.driver_system)
                    detail.text = listOf(
                        if (NativeBridge.isLoaded()) try {
                            NativeBridge.nativeDriverDescription()
                        } catch (_: Throwable) {
                            ""
                        } else "",
                        verdictText(DriverCatalog.SYSTEM),
                    ).filter { it.isNotEmpty() }.joinToString("\n")
                    chosen = chosenDirectory.isEmpty()
                }
                position <= drivers.size -> {
                    val driver = drivers[position - 1]
                    title.text = driver.name
                    detail.text = buildString {
                        if (driver.version.isNotEmpty()) append(driver.version)
                        if (driver.vendor.isNotEmpty()) {
                            if (isNotEmpty()) append(" · ")
                            append(driver.vendor)
                        }
                        if (driver.author.isNotEmpty()) {
                            if (isNotEmpty()) append(" · ")
                            append(driver.author)
                        }
                        if (driver.description.isNotEmpty()) {
                            if (isNotEmpty()) append('\n')
                            append(driver.description)
                        }
                        val verdict = verdictText(store.idOf(driver, prefs, catalog))
                        if (verdict.isNotEmpty()) {
                            if (isNotEmpty()) append('\n')
                            append(verdict)
                        }
                    }
                    chosen = chosenDirectory == driver.directory.absolutePath
                }
                else -> {
                    val entry = offered[position - 1 - drivers.size]
                    title.text = "${entry.name} · ${getString(R.string.driver_get, megabytes(entry.size))}"
                    detail.text = buildString {
                        append(entry.version).append(" · ").append(entry.source)
                        if (entry.kind == "qualcomm") append('\n').append(getString(R.string.driver_proprietary))
                        val verdict = verdictText(entry.id)
                        if (verdict.isNotEmpty()) append('\n').append(verdict)
                    }
                }
            }
            row.alpha = if (chosen) 1f else 0.7f
            row.setBackgroundResource(if (chosen) R.drawable.bg_card_selected else R.drawable.bg_card)
            return row
        }
    }
}
