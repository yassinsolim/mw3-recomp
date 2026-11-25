package com.mw2.recomp

import android.Manifest
import android.app.Activity
import android.app.DownloadManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
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
import androidx.core.content.ContextCompat
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.security.MessageDigest
import kotlin.concurrent.thread

/**
 * "Build the game code on this device": the screen of GameBuild.kt, for a
 * player with no PC (docs/ondevice-build.md). Three things in order, each
 * said as done or with the one button that does it:
 *
 *  1. the disc's executables, from the installed game files where they are
 *     there already, from a disc image found on the device or a memory card,
 *     or out of the one the player picks; the player is never asked for
 *     the executables themselves;
 *  2. the toolchain package (Toolchain.kt), downloaded from the app's own
 *     release and installed through the system's prompt;
 *  3. the build, which GameBuildService keeps going with the app in the
 *     background: this screen only shows GameBuild.progress and asks the
 *     service to start, pause or carry on.
 */
class BuildActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs
    private lateinit var state: GameBuildService.State
    private lateinit var status: TextView
    private lateinit var pick: Button
    private lateinit var browse: Button
    private lateinit var toolchainButton: Button
    private lateinit var begin: Button
    private lateinit var progress: ProgressBar
    private lateinit var progressText: TextView
    private lateinit var warning: TextView

    private val handler = Handler(Looper.getMainLooper())

    /** The executables are being taken out of the disc image or the game folder. */
    @Volatile
    private var taking = false
    private var executables = false
    private var toolchainDownload = -1L

    /** A build was running when this screen last looked: its end is news to say. */
    private var sawRunning = false

    /**
     * The package this app builds with; one of another version is said, and
     * replaced by the download. These are read when the screen comes to the
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

    private val downloadDone = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val id = toolchainDownload
            if (intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1) != id) return
            // Hashing 43 MB is not for the main thread; the screen says
            // "downloading" until the file is known to be the release's.
            thread(name = "build-toolchain-check") {
                val whole = toolchainArrived(id)
                handler.post {
                    toolchainDownload = -1
                    if (whole) installToolchain(id) else toolchainFailed()
                }
            }
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            show()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_build)
        title = getString(R.string.build_title)
        prefs = Prefs(this)
        state = GameBuildService.State(this)
        status = findViewById(R.id.status)
        pick = findViewById(R.id.pick)
        browse = findViewById(R.id.browse)
        toolchainButton = findViewById(R.id.toolchain)
        begin = findViewById(R.id.begin)
        progress = findViewById(R.id.progress)
        progressText = findViewById(R.id.progress_text)
        warning = findViewById(R.id.warning)

        pick.setOnClickListener {
            document.launch(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"))
        }
        browse.setOnClickListener { browser.launch(Intent(this, FileBrowserActivity::class.java)) }
        toolchainButton.setOnClickListener { downloadToolchain() }
        begin.setOnClickListener {
            when {
                GameBuild.progress.running -> GameBuildService.pause(this)
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
                    !state.wanted -> notifications.launch(Manifest.permission.POST_NOTIFICATIONS)
                else -> startBuild()
            }
        }
        // Exported: the broadcast is the system's download provider's (MainActivity.downloadDone).
        ContextCompat.registerReceiver(
            this, downloadDone, IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE), ContextCompat.RECEIVER_EXPORTED,
        )
    }

    override fun onDestroy() {
        unregisterReceiver(downloadDone)
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        GameBuildService.resumeIfInterrupted(this)
        reload()
        // The game files hold both executables once they are installed: no
        // disc image is asked for then. Hashing them is off the main thread.
        thread(name = "build-executables") {
            var have = GameBuild.haveExecutables(this)
            val folder = prefs.gameDirectory
            if (!have && !taking && Title.carried.all { it.installed(folder) }) {
                have = try {
                    GameBuild.takeFromGameFolder(this, folder)
                    true
                } catch (_: IOException) {
                    // Not a build this app supports: the player may have another image.
                    false
                }
            }
            // A disc image of this game anywhere on the device or a memory
            // card (DiscImage.search), which the launcher looks for too: the
            // first whose executables are a version this app supports is
            // used unasked, since it is only read. Nothing is found where
            // the app may not list folders; the player chooses one then.
            if (!have && !taking) {
                have = DiscImage.search(this).games.any { takeFromImageFile(it) }
            }
            handler.post { executables = have }
        }
        handler.post(tick)
    }

    /** Takes the executables out of [image] and remembers it for Install & Play; false when it is not a version this app supports. */
    private fun takeFromImageFile(image: File): Boolean = try {
        FileInputStream(image).channel.use { GameBuild.takeFromDiscImage(this, it) }
        prefs.chosenImage = image.path
        true
    } catch (_: IOException) {
        false
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
            val found = DiscImage.inFolder(this, folder).games.any { takeFromImageFile(it) }
            handler.post {
                taking = false
                executables = found
                if (!found && !isFinishing) {
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
                if (problem == null && image != null) prefs.chosenImage = image
                if (problem != null && !isFinishing) {
                    AlertDialog.Builder(this).setMessage(problem).setPositiveButton(R.string.close, null).show()
                }
            }
        }
    }

    private fun startBuild() {
        GameBuildService.start(this)
    }

    // ---- the toolchain package --------------------------------------------------

    private fun toolchainPage(): Uri = Uri.parse(kit.toolchainPage)

    private fun toolchainFile(): File = File(getExternalFilesDir(null), kit.toolchainAsset)

    private fun downloadToolchain() {
        if (!packageManager.canRequestPackageInstalls()) {
            // The system's own screen for allowing this app to install one;
            // the player comes back here and asks again.
            try {
                startActivity(Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            } catch (_: Exception) {
            }
            return
        }
        try {
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            // Into the app's own folder: no permission, and gone with the app.
            toolchainFile().delete()
            val request = DownloadManager.Request(Uri.parse(BuildConfig.TOOLCHAIN_URL.ifEmpty { kit.toolchainDownload }))
                .setTitle(getString(R.string.build_toolchain_name))
                .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE)
                .setDestinationInExternalFilesDir(this, null, kit.toolchainAsset)
                .setAllowedOverMetered(true)
            toolchainDownload = manager.enqueue(request)
        } catch (_: Exception) {
            openToolchainPage()
        }
    }

    /**
     * Whether download [id] ended well and brought the file the app's list
     * names (setup/toolchain.json): anything else, a page of a proxy or a
     * package of another release, is deleted and never shown to the installer.
     */
    private fun toolchainArrived(id: Long): Boolean {
        val file = toolchainFile()
        val whole = try {
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            val succeeded = manager.query(DownloadManager.Query().setFilterById(id)).use { cursor ->
                cursor.moveToFirst() &&
                    cursor.getInt(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS)) == DownloadManager.STATUS_SUCCESSFUL
            }
            succeeded && sha256(file) == kit.toolchainHash
        } catch (_: Exception) {
            false
        }
        if (!whole) file.delete()
        return whole
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun installToolchain(id: Long) {
        try {
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            val uri = manager.getUriForDownloadedFile(id) ?: throw IOException("the download failed")
            startActivity(
                Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION),
            )
        } catch (_: Exception) {
            toolchainFailed()
        }
    }

    private fun toolchainFailed() {
        if (isFinishing) return
        AlertDialog.Builder(this)
            .setMessage(getString(R.string.build_toolchain_failed, kit.toolchainAsset))
            .setPositiveButton(R.string.open_page) { _, _ -> openToolchainPage() }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openToolchainPage() {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, toolchainPage()))
        } catch (_: Exception) {
        }
    }

    // ---- what the screen says ---------------------------------------------------

    private fun charging(): Boolean {
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        return (battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
    }

    private fun reload() {
        val installed = Toolchain.find(this)
        toolchain = installed?.takeIf { kit.takes(it) }
        otherToolchain = installed != null && toolchain == null
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
                toolchainDownload != -1L -> getString(R.string.build_toolchain_downloading)
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
        toolchainButton.visibility = if (toolchain != null || busy) View.GONE else View.VISIBLE
        toolchainButton.isEnabled = toolchainDownload == -1L

        begin.visibility = if (wanted.isEmpty() && !busy) View.GONE else View.VISIBLE
        begin.isEnabled = busy || (executables && toolchain != null)
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
            progressText.text = if (state.wanted) getString(R.string.build_paused) else ""
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
