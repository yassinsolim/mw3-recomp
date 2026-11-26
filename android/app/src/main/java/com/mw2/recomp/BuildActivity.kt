package com.mw2.recomp

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import kotlin.concurrent.thread

/**
 * "Build the game code on this device": the screen of GameBuild.kt, for a
 * player with no PC (docs/ondevice-build.md). Three things in order, each
 * said as done or as what will be done:
 *
 *  1. the disc's executables, from the installed game files where they are
 *     there already, from a disc image found on the device or a memory card,
 *     or out of the one the player picks; the player is never asked for
 *     the executables themselves;
 *  2. the toolchain package (Toolchain.kt), which the build downloads from
 *     the app's own release and installs (ToolchainInstall.kt); where
 *     Android asks the player whether it may, this screen shows the question;
 *  3. the build, which GameBuildService keeps going with the app in the
 *     background: this screen only shows GameBuild.progress and asks the
 *     service to start, pause or carry on.
 *
 * A build the app started by itself after an update (GameUpdate.kt) is
 * shown here as any other.
 */
class BuildActivity : AppCompatActivity() {

    private lateinit var state: GameBuildService.State
    private lateinit var status: TextView
    private lateinit var pick: Button
    private lateinit var browse: Button
    private lateinit var begin: Button
    private lateinit var progress: ProgressBar
    private lateinit var progressText: TextView
    private lateinit var warning: TextView

    private val handler = Handler(Looper.getMainLooper())

    /** The executables are being taken out of the disc image or the game folder. */
    @Volatile
    private var taking = false
    private var executables = false

    /** A build was running when this screen last looked: its end is news to say. */
    private var sawRunning = false

    /**
     * The package this app builds with; one of another version is said, and
     * replaced by the build. These are read when the screen comes to the
     * front and when a build ends, not on every tick: they are files.
     */
    private var toolchain: Toolchain? = null
    private var otherToolchain = false

    /** The launcher offers this screen only in an app that has a build kit (GameBuild.offered). */
    private val kit: GameBuild.Kit by lazy { checkNotNull(GameBuild.kit(this)) { "this app has no build kit" } }
    private var wanted: List<GameBuild.KitTitle> = emptyList()
    private var outdated = false

    private val document = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val uri = result.data?.data
        if (result.resultCode == Activity.RESULT_OK && uri != null) {
            // The permission is kept: the image is offered to Install & Play.
            DiscImage.keep(this, uri)
            takeExecutables(uri.toString()) {
                val descriptor = contentResolver.openFileDescriptor(uri, "r") ?: throw IOException("the file could not be opened")
                descriptor.use { FileInputStream(it.fileDescriptor).channel.use { image -> GameBuild.takeFromDiscImage(this, image) } }
            }
        }
    }

    private val browser = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val path = result.data?.getStringExtra(FileBrowserActivity.EXTRA_PATH)
        if (result.resultCode == Activity.RESULT_OK && path != null) {
            if (File(path).isDirectory) takeFromFolder(File(path))
            else takeExecutables(path) { FileInputStream(path).channel.use { image -> GameBuild.takeFromDiscImage(this, image) } }
        }
    }

    /** The notification is where the player sees the build from elsewhere; it runs without the permission too. */
    private val notifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) { startBuild() }

    private val tick = object : Runnable {
        override fun run() {
            // Android's question about the toolchain package, which a build
            // in the background could not put (ToolchainInstall).
            ToolchainInstall.ask(this@BuildActivity)
            show()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_build)
        title = getString(R.string.build_title)
        state = GameBuildService.State(this)
        status = findViewById(R.id.status)
        pick = findViewById(R.id.pick)
        browse = findViewById(R.id.browse)
        begin = findViewById(R.id.begin)
        progress = findViewById(R.id.progress)
        progressText = findViewById(R.id.progress_text)
        warning = findViewById(R.id.warning)

        pick.setOnClickListener {
            document.launch(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"))
        }
        browse.setOnClickListener { browser.launch(Intent(this, FileBrowserActivity::class.java)) }
        begin.setOnClickListener {
            when {
                GameBuild.progress.running -> GameBuildService.pause(this)
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
                    !state.wanted -> notifications.launch(Manifest.permission.POST_NOTIFICATIONS)
                else -> startBuild()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        GameBuildService.resumeIfInterrupted(this)
        reload()
        // Wherever this device has them (GameBuild.findExecutables): no disc
        // image is asked for then. Hashing and searching are off the main thread.
        if (!taking) {
            thread(name = "build-executables") {
                val have = GameBuild.findExecutables(this)
                handler.post { if (!taking) executables = have }
            }
        }
        handler.post(tick)
    }

    /**
     * A folder the player chose in the browser: one holding a disc's
     * extracted files, or the one their disc image lies in, which is what a
     * player who keeps the image in a folder of its own means by it.
     */
    private fun takeFromFolder(folder: File) {
        if (GameBuild.inGameFolder(this, folder)) {
            takeExecutables(null) { GameBuild.takeFromGameFolder(this, folder.path) }
            return
        }
        taking = true
        thread(name = "build-executables") {
            val found = DiscImage.inFolder(this, folder).games.firstOrNull { GameBuild.takeFromImage(this, it.path) }
            handler.post {
                taking = false
                executables = found != null
                // Remembered for Install & Play.
                if (found != null) Prefs(this).chosenImage = found.path
                if (found == null && !isFinishing) {
                    AlertDialog.Builder(this)
                        .setMessage(getString(R.string.build_folder_nothing, folder.path))
                        .setPositiveButton(R.string.close, null)
                        .show()
                }
            }
        }
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    /** Runs [take]; [image] is the disc image it reads, remembered for Install & Play when it was this game's. */
    private fun takeExecutables(image: String?, take: () -> Unit) {
        taking = true
        thread(name = "build-executables") {
            val problem = try {
                take()
                null
            } catch (error: Exception) {
                error.message ?: error.toString()
            }
            handler.post {
                taking = false
                executables = problem == null
                if (problem == null && image != null) Prefs(this).chosenImage = image
                if (problem != null && !isFinishing) {
                    AlertDialog.Builder(this).setMessage(problem).setPositiveButton(R.string.close, null).show()
                }
            }
        }
    }

    private fun startBuild() {
        GameBuildService.start(this, automatic = false)
    }

    // ---- what the screen says ---------------------------------------------------

    private fun charging(): Boolean {
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        return (battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
    }

    private fun reload() {
        val installed = Toolchain.find(this)
        toolchain = installed?.takeIf { kit.takes(it) }
        otherToolchain = toolchain == null && Toolchain.present(this)
        wanted = GameBuild.wanted(this)
        outdated = GameBuild.outdated(this)
    }

    private fun show() {
        val now = GameBuild.progress
        if (sawRunning && !now.running) reload()
        val toolchain = toolchain
        val lines = ArrayList<String>()
        lines.add(
            getString(
                when {
                    executables -> R.string.build_have_executables
                    taking -> R.string.build_taking_executables
                    else -> R.string.build_need_executables
                },
            ),
        )
        lines.add(
            when {
                toolchain != null -> getString(R.string.build_have_toolchain, toolchain.version)
                ToolchainInstall.confirmation != null -> getString(R.string.build_toolchain_asked)
                otherToolchain -> getString(R.string.build_other_toolchain, kit.toolchainVersion)
                else -> getString(R.string.build_need_toolchain)
            },
        )
        if (wanted.isEmpty()) lines.add(getString(R.string.build_done))
        status.text = lines.joinToString("\n")

        val busy = now.running || state.wanted
        pick.visibility = if (executables || busy) View.GONE else View.VISIBLE
        browse.visibility = pick.visibility
        pick.isEnabled = !taking
        browse.isEnabled = !taking

        begin.visibility = if (wanted.isEmpty() && !busy) View.GONE else View.VISIBLE
        begin.isEnabled = busy || executables
        begin.text = getString(
            when {
                now.running -> R.string.build_pause
                state.wanted -> R.string.build_resume
                outdated -> R.string.build_again
                else -> R.string.build_begin
            },
        )

        progress.visibility = if (busy) View.VISIBLE else View.GONE
        if (now.running) {
            progress.isIndeterminate = now.total == 0
            progress.max = maxOf(1, now.total)
            progress.progress = now.done
            var said = GameBuildService.describe(this, now)
            if (now.step == GameBuild.Step.COMPILE) {
                said += "\n" + getString(
                    when (now.held) {
                        "memory" -> R.string.build_jobs_memory
                        "heat" -> R.string.build_jobs_heat
                        else -> R.string.build_jobs
                    },
                    now.jobs, now.batteryCelsius,
                )
            }
            progressText.text = said
        } else {
            progressText.text = when {
                !state.wanted -> ""
                state.paused -> getString(R.string.build_paused)
                ToolchainInstall.confirmation != null -> getString(R.string.update_code_confirm)
                else -> getString(R.string.build_starting)
            }
        }
        warning.visibility = if (busy && !charging()) View.VISIBLE else View.GONE

        val ended = sawRunning && !now.running
        sawRunning = now.running
        // A failure is said when it happens, or when the player comes back
        // to this screen after one that happened while they were elsewhere.
        val error = state.error
        if (!now.running && error.isNotEmpty()) {
            state.error = ""
            AlertDialog.Builder(this)
                .setTitle(R.string.build_failed)
                .setMessage(error + "\n\n" + getString(R.string.build_failed_where, GameBuild.report(this).path))
                .setPositiveButton(R.string.close, null)
                .show()
        } else if (ended && !state.wanted && wanted.isEmpty()) {
            AlertDialog.Builder(this)
                .setTitle(R.string.build_title)
                .setMessage(R.string.build_done_explained)
                .setPositiveButton(R.string.close) { _, _ -> finish() }
                .setCancelable(false)
                .show()
        }
    }
}
