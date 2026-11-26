package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.IOException
import kotlin.concurrent.thread

/**
 * Add DLC: the player's own map packs and title update, copied off their
 * console's hard disk (docs/dlc.md), chosen as a folder (the console's
 * Content\0000000000000000\41560914, or wherever the files were put) or as
 * files, and copied into the dlc folder (Dlc.kt). Lists what is there; a long
 * press removes one.
 *
 * The map packs are for the game the title update makes, whose game code is
 * built from the player's disc and that update: once the update's package is
 * here the build starts by itself, on this device and in the background
 * (GameBuild.kt, GameBuildService.kt), and this screen says how far it is.
 */
class DlcActivity : AppCompatActivity() {

    private lateinit var status: TextView
    private lateinit var list: ListView
    private lateinit var progress: ProgressBar
    private lateinit var addFolder: Button
    private lateinit var addFiles: Button
    private lateinit var build: Button
    private var packages: List<Dlc.Package> = emptyList()

    @Volatile
    private var cancelled = false

    /** While the game code is being built: its progress, said again every two seconds. */
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            refresh()
        }
    }

    private val pickFolder = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val tree = result.data?.data ?: return@registerForActivityResult
        add { Dlc.filesUnder(this, tree) }
    }

    private val pickFiles = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val data = result.data ?: return@registerForActivityResult
        val chosen = ArrayList<Uri>()
        data.clipData?.let { clip -> (0 until clip.itemCount).mapTo(chosen) { clip.getItemAt(it).uri } }
        if (chosen.isEmpty()) data.data?.let { chosen.add(it) }
        add { chosen.map { Dlc.single(this, it) } }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_dlc)
        title = getString(R.string.dlc_title)
        status = findViewById(R.id.dlc_status)
        list = findViewById(R.id.dlc_list)
        progress = findViewById(R.id.dlc_progress)
        addFolder = findViewById(R.id.dlc_add_folder)
        addFiles = findViewById(R.id.dlc_add_files)
        build = findViewById(R.id.dlc_build)
        build.setOnClickListener {
            GameBuildService.start(this, automatic = false)
            refresh()
        }
        findViewById<TextView>(R.id.dlc_hint).text =
            getString(if (Dlc.takesUpdate) R.string.dlc_hint_update else R.string.dlc_hint, Dlc.consoleFolder)

        addFolder.setOnClickListener { pickFolder.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)) }
        addFiles.setOnClickListener {
            // The console's packages have no extension and no type a picker knows.
            pickFiles.launch(
                Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true),
            )
        }
        list.setOnItemLongClickListener { _, _, position, _ ->
            val item = packages[position]
            AlertDialog.Builder(this)
                .setTitle(item.header.name)
                .setMessage(getString(R.string.dlc_remove_ask, item.file.length() / 1e9))
                .setPositiveButton(R.string.dlc_remove) { _, _ ->
                    Dlc.remove(this, item)
                    refresh()
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
            true
        }
        refresh()
    }

    override fun onResume() {
        super.onResume()
        // A build that waited for the player's answer about the toolchain package, or that the system ended.
        GameBuildService.resumeIfInterrupted(this)
        refresh()
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    override fun onDestroy() {
        cancelled = true
        super.onDestroy()
    }

    private fun refresh() {
        TitleUpdate.refresh(this)
        packages = Dlc.installed(this)
        val packs = packages.count { !it.header.update }
        val buildable = TitleUpdate.codeMissing && GameBuild.offered(this)
        val state = GameBuildService.State(this)
        val building = buildable && (state.wanted || GameBuild.progress.running)
        // The system's picker hands its answer over before this screen is
        // resumed, and a resume looks again: a copy under way keeps its word.
        status.text = if (progress.visibility == View.VISIBLE) getString(R.string.dlc_copying) else when {
            packages.isEmpty() -> getString(R.string.dlc_none)
            // The disc's executables read the packs themselves. Modern Warfare 3's
            // were played from real ones; the other two games' never were (docs/dlc.md).
            !Dlc.takesUpdate -> getString(
                when {
                    !Title.isMw3 -> R.string.dlc_packs_untested
                    Dlc.levels(this).isNotEmpty() -> R.string.dlc_packs_ready_levels
                    else -> R.string.dlc_packs_ready
                },
                packs,
            )
            TitleUpdate.ready -> getString(R.string.dlc_ready, packs)
            building -> getString(
                R.string.dlc_building, packs,
                if (ToolchainInstall.confirmation != null) getString(R.string.update_code_confirm)
                else GameBuildService.describe(this, GameBuild.progress),
            )
            buildable && state.error.isNotEmpty() -> getString(R.string.dlc_build_failed, packs, state.error)
            TitleUpdate.codeMissing -> getString(R.string.dlc_code_missing, packs, getString(R.string.setup_command))
            // A game that is nothing without its update counts its packs before the update is here.
            TitleUpdate.updateMissing -> getString(R.string.dlc_update_missing, packs)
            // Everything but a Compatibility Pack: the note below is the whole of it.
            TitleUpdate.missingPacks.isNotEmpty() -> getString(R.string.dlc_update_here, packs)
            else -> getString(R.string.dlc_update_missing, packs)
        } + packsMissing(packages)
        build.visibility = if (buildable && !building) View.VISIBLE else View.GONE
        handler.removeCallbacks(tick)
        if (building) handler.postDelayed(tick, 2000)
        list.adapter = ArrayAdapter(
            this, android.R.layout.simple_list_item_1,
            packages.map { item ->
                getString(
                    if (item.header.update) R.string.dlc_item_update else R.string.dlc_item_pack,
                    item.header.name, item.file.length() / 1e9,
                )
            },
        )
    }

    /**
     * Which Compatibility Packs are still to add, as a paragraph after what
     * else is said, or nothing: the title's own word for it is a notice
     * that they are being downloaded, and nothing downloads here.
     */
    private fun packsMissing(packages: List<Dlc.Package>): String {
        if (packages.isEmpty()) return ""
        val missing = Dlc.missingCompatibilityPacks(packages)
        if (missing.isEmpty()) return ""
        return "\n\n" + getString(R.string.dlc_packs_missing, Dlc.compatibilityPackNames(this, missing))
    }

    /** Copies what [find] names, off the main thread: a pack is a gigabyte and more. */
    private fun add(find: () -> List<Dlc.Source>) {
        cancelled = false
        addFolder.isEnabled = false
        addFiles.isEnabled = false
        progress.progress = 0
        progress.visibility = View.VISIBLE
        status.text = getString(R.string.dlc_copying)
        thread(name = "add-dlc") {
            val message = try {
                val added = Dlc.add(
                    this, find(),
                    { done, total -> runOnUiThread { progress.progress = if (total > 0) (done * 1000 / total).toInt() else 0 } },
                    { cancelled },
                )
                // The update's package is new here and its game code is not:
                // the player asked for the map packs, so it is built now.
                if (TitleUpdate.codeMissing && GameBuild.offered(this) && !GameBuildService.State(this).wanted) {
                    runOnUiThread { if (!isFinishing && !isDestroyed) GameBuildService.start(this, automatic = false) }
                }
                when {
                    added.copied.isNotEmpty() ->
                        getString(R.string.dlc_added, added.copied.joinToString(", ")) + packsMissing(Dlc.installed(this))
                    added.already > 0 -> getString(R.string.dlc_already)
                    else -> getString(
                        if (Dlc.takesUpdate) R.string.dlc_nothing_found_update else R.string.dlc_nothing_found, Dlc.consoleFolder,
                    )
                }
            } catch (error: IOException) {
                error.message ?: error.toString()
            } catch (error: SecurityException) {
                error.message ?: error.toString()
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                progress.visibility = View.GONE
                addFolder.isEnabled = true
                addFiles.isEnabled = true
                refresh()
                AlertDialog.Builder(this).setTitle(R.string.dlc_title).setMessage(message).setPositiveButton(R.string.close, null).show()
            }
        }
    }
}
