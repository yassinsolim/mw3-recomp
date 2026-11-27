package com.mw2.recomp

import android.app.Activity
import android.app.DownloadManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.StatFs
import android.provider.OpenableColumns
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.File
import kotlin.concurrent.thread

/**
 * Setting the player's own copy of the game up to be played.
 *
 * Two ways, and the player chooses; the choice is kept (Prefs.installByCopy):
 *
 *   From the image   the game reads its files out of the disc image where
 *                    it lies (runtime/kernel/volume.h), on the device or on
 *                    a memory card. Only the executables are copied, which
 *                    takes a second; the image has to stay where it is.
 *   Copied           what the game needs is copied into the app's storage,
 *                    about 7 GB, from an image or from a folder a disc was
 *                    extracted to; the source is not needed afterwards.
 *
 * Advanced Warfare is two discs, the game disc and the install disc, and
 * both go the same way: played from the images, the install disc's two
 * packages are read out of its image as well (runtime/kernel/content.cpp).
 * They are chosen together -- found beside each other, in the folder the
 * player chose, or both picked in the Files app -- or one after the other.
 *
 * The work is the runtime's (runtime/install/install.cpp): it reads a disc
 * image or a folder, checks the executables against the disc this build was
 * made from, and can be interrupted and carried on later. This screen is a
 * source, the choice, a progress bar and a cancel button.
 *
 * Two ways to name the source, because Android has two and neither covers
 * everyone:
 *
 *   Files      the system picker. No permission at all, works with a phone
 *              whose storage is locked down and with a memory card. It
 *              hands over a document rather than a path: a file on the
 *              device is read through it where it lies (DiscImage.kt), and
 *              only one that is not a file here, a cloud entry, has to be
 *              copied into the app's folder first.
 *   Browse     a plain directory listing over real paths, and the only way
 *              to name a folder of already-extracted files -- but reading
 *              outside the app's own folder needs the all-files permission.
 */
class InstallActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs
    private lateinit var sourceText: TextView
    private lateinit var progress: ProgressBar
    private lateinit var progressText: TextView
    private lateinit var begin: Button
    private lateinit var choose: Button
    private lateinit var pick: Button
    private lateinit var cancel: Button
    private lateinit var linkMode: android.widget.RadioButton
    private lateinit var copyMode: android.widget.RadioButton
    private lateinit var modeNote: TextView

    private val handler = Handler(Looper.getMainLooper())
    /** What the game is installed from: a path, of an image or of a folder of a disc's files, or a document's address (DiscImage.kt). */
    private var source: String = ""

    /** Advanced Warfare's second disc, the install disc, named as [source] is; set up after it. */
    private var installDisc: String = ""

    /** Whether the install running, or the last to finish, plays from the images. */
    private var linked = false

    /** What that install read, in its order. */
    private var ran: List<String> = emptyList()

    /** The bytes of earlier copies the app still holds once it plays from the images. */
    private var copies = 0L

    @Volatile
    private var running = false

    @Volatile
    private var copying = false

    @Volatile
    private var activeCopyStream: java.io.InputStream? = null
    private var tuDownloadId: Long = -1
    private var tuTarget: File? = null

    private val values = LongArray(2)

    private val browse = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val path = result.data?.getStringExtra(FileBrowserActivity.EXTRA_PATH)
            ?: return@registerForActivityResult
        useSource(path)
    }

    private val document = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val several = result.data?.clipData
        if (several != null) {
            for (index in 0 until several.itemCount) chooseDocument(several.getItemAt(index).uri)
        } else {
            chooseDocument(result.data?.data ?: return@registerForActivityResult)
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            val file = NativeBridge.nativeInstallProgress(values)
            val done = values[0]
            val total = values[1]
            val percent = if (total > 0) ((done * 100) / total).toInt() else 0
            progress.progress = if (total > 0) ((done * 1000) / total).toInt() else 0
            progressText.text = getString(R.string.installing, file, percent)
            handler.postDelayed(this, 250)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_install)
        prefs = Prefs(this)
        title = getString(R.string.install_game)

        sourceText = findViewById(R.id.source)
        progress = findViewById(R.id.progress)
        progressText = findViewById(R.id.progress_text)
        begin = findViewById(R.id.begin)
        choose = findViewById(R.id.choose)
        pick = findViewById(R.id.pick)
        cancel = findViewById(R.id.cancel)
        linkMode = findViewById(R.id.mode_link)
        copyMode = findViewById(R.id.mode_copy)

        modeNote = findViewById(R.id.mode_note)
        sourceText.text = getString(R.string.no_source)
        // The player's own choice, kept: a tap, not what the screen sets.
        linkMode.setOnClickListener { prefs.installByCopy = false }
        copyMode.setOnClickListener { prefs.installByCopy = true }
        if (prefs.installByCopy) copyMode.isChecked = true

        pick.setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            intent.addCategory(Intent.CATEGORY_OPENABLE)
            intent.type = "*/*"
            // Advanced Warfare's two discs can be chosen in one go.
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, Title.isAw)
            document.launch(intent)
        }
        choose.setOnClickListener {
            browse.launch(Intent(this, FileBrowserActivity::class.java))
        }
        begin.setOnClickListener { start() }
        cancel.setOnClickListener {
            if (running && NativeBridge.isLoaded()) NativeBridge.nativeCancelInstall()
            if (tuDownloadId != -1L) {
                try {
                    (getSystemService(DOWNLOAD_SERVICE) as DownloadManager).remove(tuDownloadId)
                } catch (_: Throwable) {
                }
                tuDownloadId = -1
                tuTarget = null
                progress.isIndeterminate = false
            }
            copying = false
            try { activeCopyStream?.close() } catch (_: Throwable) {}
            cancel.isEnabled = false
        }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!running && !copying) {
                    finish()
                    return
                }
                AlertDialog.Builder(this@InstallActivity)
                    .setMessage(R.string.install_running_warning)
                    .setNegativeButton(R.string.cancel, null)
                    .setPositiveButton(R.string.close) { _, _ ->
                        copying = false
                        try { activeCopyStream?.close() } catch (_: Throwable) {}
                        if (NativeBridge.isLoaded()) NativeBridge.nativeCancelInstall()
                    }
                    .show()
            }
        })

        // What the launcher of an app of one title found.
        intent.getStringExtra(EXTRA_INITIAL_PATH)?.let { found ->
            if (found.isNotEmpty() && (DiscImage.isDocument(found) || File(found).exists())) useSource(found)
        }
        if ((source.isNotEmpty() || installDisc.isNotEmpty()) && intent.getBooleanExtra(EXTRA_AUTO_START, false)) {
            start()
        }
        ContextCompat.registerReceiver(
            this, tuDownloadDone,
            android.content.IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
    }

    private val tuDownloadDone = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val id = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1)
            if (id != tuDownloadId) return
            tuDownloadId = -1
            applyDownloadedTu()
        }
    }

    companion object {
        const val EXTRA_INITIAL_PATH = "initial_path"
        const val EXTRA_AUTO_START = "auto_start"

        /** The title the launcher was asked for, or the app's first. */
        const val EXTRA_TITLE = "title"
    }

    private fun isFolder(image: String): Boolean = !DiscImage.isDocument(image) && File(image).isDirectory

    /** Whether [folder] is Advanced Warfare's install disc extracted: it has the folder the packages are in. */
    private fun isInstallDiscFolder(folder: File): Boolean =
        Title.isAw && File(folder, "content/0000000000000000/41560914/00000002").isDirectory

    /**
     * Takes what the player chose, or the launcher found. A disc image is
     * told by what is in it (DiscImage.kind): Advanced Warfare's install
     * disc goes beside the game disc, whichever was chosen first. A folder
     * is either a disc's files extracted, which can only be copied, or
     * simply where the images lie -- a player who keeps both discs in one
     * folder chooses that folder -- and then the images in it are taken.
     */
    private fun useSource(image: String) {
        if (isFolder(image)) {
            val folder = File(image)
            when {
                isInstallDiscFolder(folder) -> installDisc = image
                Title.carried.any { File(folder, it.executable).isFile } -> source = image
                else -> {
                    val found = DiscImage.inFolder(this, folder)
                    if (found.games.isEmpty() && found.installs.isEmpty()) {
                        AlertDialog.Builder(this)
                            .setMessage(getString(R.string.folder_nothing, image))
                            .setPositiveButton(R.string.close, null)
                            .show()
                        return
                    }
                    found.games.firstOrNull()?.let { source = it.path }
                    found.installs.firstOrNull()?.let { installDisc = it.path }
                }
            }
        } else if (DiscImage.kindOf(this, image) == DiscImage.Kind.INSTALL) {
            installDisc = image
        } else {
            // Whatever else it is: the installer says what is wrong with an
            // image that is not this game's (runtime/install/install.cpp).
            source = image
            // The install disc usually lies beside the game disc, and is
            // taken with it; a document has no folder to look in.
            if (Title.isAw && installDisc.isEmpty() && !DiscImage.isDocument(image)) {
                File(image).parentFile?.let { DiscImage.inFolder(this, it).installs.firstOrNull() }
                    ?.let { installDisc = it.path }
            }
        }
        showSources()
    }

    /** What is chosen, and which of the two ways it can be set up. */
    private fun showSources() {
        sourceText.text = when {
            source.isEmpty() && installDisc.isEmpty() -> getString(R.string.no_source)
            !Title.isAw -> getString(R.string.chosen, DiscImage.label(this, source))
            else -> {
                val target = gameDirectory().absolutePath
                getString(
                    R.string.chosen_two,
                    discLabel(source, safelyInstalled(gameDirectory())),
                    discLabel(installDisc, !Title.installDiscMissing(target, prefs.installDiscImage)),
                )
            }
        }
        // A folder of extracted files has no image to play from. The
        // player's choice is kept for the next image all the same: only
        // what they tap changes it (Prefs.installByCopy).
        val extracted = isFolder(source) || isFolder(installDisc)
        linkMode.isEnabled = !extracted
        modeNote.visibility = if (extracted) View.VISIBLE else View.GONE
        if (extracted || prefs.installByCopy) copyMode.isChecked = true else linkMode.isChecked = true
        begin.isEnabled = NativeBridge.isLoaded() && (source.isNotEmpty() || installDisc.isNotEmpty())
    }

    /** One of Advanced Warfare's two discs as the source's box names it. */
    private fun discLabel(image: String, done: Boolean): String = when {
        image.isNotEmpty() -> DiscImage.label(this, image)
        done -> getString(R.string.disc_already_set_up)
        else -> getString(R.string.disc_not_chosen)
    }

    private fun gameDirectory(): File {
        // The app's own external folder: no permission needed to write it,
        // it survives an update, and it goes when the app is uninstalled --
        // which is what a player expects of seven gigabytes.
        val base = getExternalFilesDir(null) ?: filesDir
        return File(base, "game")
    }

    // ---- bringing a document in ------------------------------------------------

    /**
     * A document the picker hands over: read where it lies when it is a file
     * on this device, with the permission kept for later runs, and copied in
     * first when it is not.
     */
    private fun chooseDocument(uri: Uri) {
        DiscImage.keep(this, uri)
        val readable = DiscImage.source(this, uri.toString())
        if (readable == null) {
            importDocument(uri)
            return
        }
        DiscImage.close(readable)
        useSource(uri.toString())
    }

    /**
     * A document that is only a stream, where the runtime's installer reads
     * at positions. So it is copied in first -- with the size said out loud,
     * because for a disc image this is gigabytes.
     */
    private fun importDocument(uri: Uri) {
        val name = documentName(uri)
        val size = documentSize(uri)
        val target = File(getExternalFilesDir(null) ?: filesDir, "import")
        target.mkdirs()
        val destination = File(target, name)

        val free = StatFs(target.absolutePath).availableBytes
        val needed = size
        val message = if (size > 0) {
            getString(
                R.string.import_explained,
                android.text.format.Formatter.formatFileSize(this, size),
                android.text.format.Formatter.formatFileSize(this, free),
            )
        } else {
            getString(R.string.import_explained_unknown)
        }

        AlertDialog.Builder(this)
            .setTitle(R.string.import_file)
            .setMessage(message)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.import_file) { _, _ ->
                if (needed > 0 && free < needed) {
                    AlertDialog.Builder(this)
                        .setMessage(
                            getString(
                                R.string.no_space,
                                android.text.format.Formatter.formatFileSize(this, free),
                            )
                        )
                        .setPositiveButton(R.string.close, null)
                        .show()
                    return@setPositiveButton
                }
                copyIn(uri, destination, size)
            }
            .show()
    }

    private fun copyIn(uri: Uri, destination: File, size: Long) {
        copying = true
        setBusy(true)
        progress.isIndeterminate = size <= 0
        thread(name = "import") {
            var failure: String? = null
            var copied = 0L
            try {
                contentResolver.openInputStream(uri).use { input ->
                    if (input == null) throw IllegalStateException("no stream")
                    activeCopyStream = input
                    destination.outputStream().use { output ->
                        val buffer = ByteArray(1 shl 20)
                        var shown = 0L
                        while (copying) {
                            val read = input.read(buffer)
                            if (read <= 0) break
                            output.write(buffer, 0, read)
                            copied += read
                            if (copied - shown > 8L * 1024 * 1024) {
                                shown = copied
                                val done = copied
                                handler.post {
                                    if (size > 0) progress.progress = ((done * 1000) / size).toInt()
                                    progressText.text = getString(
                                        R.string.copying,
                                        android.text.format.Formatter.formatFileSize(this, done),
                                    )
                                }
                            }
                        }
                    }
                }
                if (!copying) {
                    destination.delete()
                    failure = getString(R.string.install_cancelled)
                } else if (size > 0 && copied < size) {
                    destination.delete()
                    failure = "Incomplete copy: expected $size bytes, got $copied bytes"
                }
            } catch (error: Throwable) {
                destination.delete()
                failure = if (!copying) getString(R.string.install_cancelled) else (error.message ?: "copy failed")
            } finally {
                activeCopyStream = null
            }
            handler.post {
                copying = false
                progress.isIndeterminate = false
                setBusy(false)
                if (failure == null) {
                    progressText.text = ""
                    useSource(destination.absolutePath)
                } else {
                    progressText.text = failure
                }
            }
        }
    }

    private fun documentName(uri: Uri): String {
        var name: String? = null
        try {
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (column >= 0 && cursor.moveToFirst()) name = cursor.getString(column)
            }
        } catch (_: Throwable) {
        }
        val cleaned = (name ?: uri.lastPathSegment ?: "source.iso")
            .substringAfterLast('/')
            .map { if (it.isLetterOrDigit() || it == '.' || it == '-' || it == '_') it else '_' }
            .joinToString("")
        return cleaned.ifEmpty { "source.iso" }
    }

    private fun documentSize(uri: Uri): Long {
        try {
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val column = cursor.getColumnIndex(OpenableColumns.SIZE)
                if (column >= 0 && cursor.moveToFirst() && !cursor.isNull(column)) {
                    return cursor.getLong(column)
                }
            }
        } catch (_: Throwable) {
        }
        return 0
    }

    // ---- installing ---------------------------------------------------------------

    private fun start() {
        if (!NativeBridge.isLoaded() || running) return
        val target = gameDirectory()
        target.mkdirs()
        val link = linkMode.isEnabled && linkMode.isChecked
        // Asked for by hand or by the launcher, the way it is done is the
        // player's choice from here on.
        if (linkMode.isEnabled) prefs.installByCopy = !link

        val free = StatFs(target.absolutePath).availableBytes
        if (!link && free < 7L * 1024 * 1024 * 1024) {
            AlertDialog.Builder(this)
                .setTitle(R.string.install_game)
                .setMessage(
                    getString(
                        R.string.no_space,
                        android.text.format.Formatter.formatFileSize(this, free),
                    )
                )
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.begin_install) { _, _ -> reallyStart(target, link) }
                .show()
            return
        }
        reallyStart(target, link)
    }

    private fun reallyStart(target: File, link: Boolean) {
        // The game disc first: Advanced Warfare's install disc comes after it.
        val queue = listOf(source, installDisc).filter { it.isNotEmpty() }
        running = true
        linked = link
        ran = queue
        setBusy(true)
        handler.post(tick)

        // The runtime's installer blocks: it may be copying gigabytes. It
        // runs on a thread of its own and the screen polls it.
        thread(name = "install") {
            var left = 0L
            var error: String? = null
            for (image in queue) {
                val from = if (isFolder(image)) image else DiscImage.source(this, image)
                if (from == null) {
                    error = getString(R.string.source_unreadable, DiscImage.label(this, image))
                    break
                }
                error = try {
                    NativeBridge.nativeInstall(from, target.absolutePath, link).also {
                        if (it == null && link) left += NativeBridge.nativeCopiedFiles(from, target.absolutePath, false)
                    }
                } catch (failure: Throwable) {
                    failure.message ?: "unexpected failure"
                }
                // The game's own process opens the image for itself.
                DiscImage.close(from)
                if (error != null) break
                // What the game reads this disc from, said as soon as it is
                // set up: each disc on its own, since one may be set up
                // without the other being touched.
                if (image == installDisc) prefs.installDiscImage = if (link) image else ""
                else prefs.discImage = if (link) image else ""
            }
            handler.post {
                copies = left
                finished(error, target)
            }
        }
    }

    private fun setBusy(busy: Boolean) {
        begin.isEnabled = !busy && (source.isNotEmpty() || installDisc.isNotEmpty()) && NativeBridge.isLoaded()
        choose.isEnabled = !busy
        pick.isEnabled = !busy
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        cancel.visibility = if (busy) View.VISIBLE else View.GONE
        cancel.isEnabled = busy
    }

    private fun finished(error: String?, target: File) {
        running = false
        handler.removeCallbacks(tick)
        setBusy(false)

        if (error == null) {
            // TU6 builds install the disc first; the update applies after.
            if (isTu6() && !safelyInstalled(target)) {
                startTuUpdate(target)
                return
            }
            // Advanced Warfare is installed from two images, together or
            // one after the other in either order: after one alone, the
            // screen asks for the other.
            if (Title.isAw) {
                val other = when {
                    !safelyInstalled(target) -> R.string.aw_game_disc_needed
                    Title.installDiscMissing(target.absolutePath, prefs.installDiscImage) -> R.string.aw_install_disc_needed
                    else -> null
                }
                if (other != null) {
                    // In the source's box too, at the top of the screen:
                    // the progress line is below the buttons, off a
                    // handheld's screen (Retroid Pocket Flip 2).
                    progressText.text = getString(other)
                    source = ""
                    installDisc = ""
                    sourceText.text = getString(other)
                    begin.isEnabled = false
                    return
                }
            }
            installedDone(target)
        } else {
            progressText.text = getString(R.string.install_failed, error)
            AlertDialog.Builder(this)
                .setTitle(R.string.install_game)
                .setMessage(getString(R.string.install_failed, error))
                .setPositiveButton(R.string.close, null)
                .show()
        }
    }

    private fun safelyInstalled(target: File): Boolean = try {
        NativeBridge.isLoaded() && NativeBridge.nativeGameInstalled(target.absolutePath)
    } catch (_: Throwable) {
        false
    }

    private fun installedDone(target: File) {
        // The images were said as each was set up, before this: the game
        // folder is what says the game is installed.
        prefs.gameDirectory = target.absolutePath
        progressText.text = getString(if (linked) R.string.install_done_link else R.string.install_done)
        // An imported copy has done its job and is the size of a disc, unless
        // it is an image the game now plays from.
        val imported = File(getExternalFilesDir(null) ?: filesDir, "import")
        if (listOf(prefs.discImage, prefs.installDiscImage).none { it.startsWith(imported.absolutePath) }) {
            imported.deleteRecursively()
        }
        if (linked && copies > 0) {
            offerToRemoveCopies(target)
            return
        }
        installedPlay()
    }

    /**
     * A game that was copied in and now plays from the image still has the
     * copies, gigabytes the image has too: the player is asked, since they
     * are also what a return to the copied game would use.
     */
    private fun offerToRemoveCopies(target: File) {
        AlertDialog.Builder(this)
            .setTitle(R.string.install_game)
            .setMessage(
                getString(R.string.copies_found, android.text.format.Formatter.formatFileSize(this, copies)),
            )
            .setCancelable(false)
            .setNegativeButton(R.string.copies_keep) { _, _ -> installedPlay() }
            .setPositiveButton(R.string.copies_remove) { _, _ ->
                setBusy(true)
                progress.isIndeterminate = true
                thread(name = "remove-copies") {
                    for (image in ran) {
                        val from = DiscImage.source(this, image) ?: continue
                        try {
                            NativeBridge.nativeCopiedFiles(from, target.absolutePath, true)
                        } catch (_: Throwable) {
                        }
                        DiscImage.close(from)
                    }
                    handler.post {
                        progress.isIndeterminate = false
                        setBusy(false)
                        installedPlay()
                    }
                }
            }
            .show()
    }

    private fun installedPlay() {
        val done = getString(if (linked) R.string.install_done_link else R.string.install_done)
        // The game's files are one half; an app published without the game's
        // code needs that too (GameLibrary.kt), and a run started without it
        // only ends in "The game could not start". Said here instead.
        // Black Ops III has a third: its title update and Compatibility Pack
        // 1, which the game code is built from and the game does not start
        // without (TitleUpdate.required).
        TitleUpdate.refresh(this)
        val updateNeeded = TitleUpdate.required && (TitleUpdate.updateMissing || TitleUpdate.missingPacks.isNotEmpty())
        val playable = !updateNeeded && GameLibrary.ready(this, titleToStart())
        if (playable && intent.getBooleanExtra(EXTRA_AUTO_START, false)) {
            GameActivity.start(this, titleToStart()) { finish() }
            return
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.install_game)
            .setNegativeButton(R.string.close) { _, _ -> finish() }
        if (playable) {
            dialog.setMessage(done)
                .setPositiveButton(R.string.play) { _, _ ->
                    GameActivity.start(this, titleToStart()) { finish() }
                }
        } else if (updateNeeded) {
            dialog.setMessage(done + "\n\n" + getString(R.string.status_update_needed))
                .setPositiveButton(R.string.dlc_title) { _, _ ->
                    startActivity(Intent(this, DlcActivity::class.java))
                    finish()
                }
        } else {
            // The launcher's large button builds it (the guided setup), or
            // says how where this app cannot.
            dialog.setMessage(done + "\n\n" + getString(if (GameBuild.offered(this)) R.string.build_code_missing else R.string.game_code_missing))
        }
        dialog.show()
    }

    /** The title the launcher was asked for, or the app's first. */
    private fun titleToStart(): Title =
        Title.carried.firstOrNull { it.name == intent.getStringExtra(EXTRA_TITLE) } ?: Title.carried.first()

    private fun isTu6(): Boolean = packageName.endsWith(".tu6")

    // ---- title update 6 (TU6 apps): downloaded like the PC launcher does ----

    private fun tuPackageFile(): File = File(getExternalFilesDir(null) ?: filesDir, "tu6.pkg")

    private fun startTuUpdate(target: File) {
        tuTarget = target
        val pkg = tuPackageFile()
        // A kept package applies offline; the URL is pinned, so it cannot go stale.
        if (pkg.exists() && pkg.length() > 100_000L) {
            applyTuPackage(pkg, target)
            return
        }
        val url = BuildConfig.MW2_TU6_URL
        if (url.isEmpty()) {
            progressText.text = getString(R.string.install_failed, "This TU6 build knows no update URL.")
            return
        }
        try {
            progressText.text = getString(R.string.tu6_downloading)
            setBusy(true)
            progress.isIndeterminate = true
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            val request = DownloadManager.Request(Uri.parse(url))
                .setTitle("Title update 6")
                .setDescription("Downloading update")
                .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE)
                .setDestinationInExternalFilesDir(this, null, "tu6.pkg")
                .setAllowedOverMetered(true)
            tuDownloadId = manager.enqueue(request)
        } catch (failure: Throwable) {
            progress.isIndeterminate = false
            setBusy(false)
            progressText.text = getString(R.string.install_failed, failure.message ?: "download failed")
        }
    }

    private fun applyDownloadedTu() {
        val target = tuTarget ?: return
        tuTarget = null
        progress.isIndeterminate = false
        setBusy(false)
        // A failed download leaves no package: say so instead of applying half a file.
        val pkg = tuPackageFile()
        if (!pkg.exists() || pkg.length() <= 100_000L) {
            progressText.text = getString(R.string.install_failed, "Title update 6 did not download.")
            return
        }
        applyTuPackage(pkg, target)
    }

    private fun applyTuPackage(pkg: File, target: File) {
        running = true
        setBusy(true)
        progress.isIndeterminate = true
        progressText.text = getString(R.string.tu6_applying)
        thread(name = "tu6-apply") {
            val error = try {
                if (!NativeBridge.isLoaded()) "The runtime is not loaded."
                else NativeBridge.nativeApplyTu6(pkg.absolutePath, target.absolutePath)
            } catch (failure: Throwable) {
                failure.message ?: "unexpected failure"
            }
            handler.post {
                running = false
                progress.isIndeterminate = false
                setBusy(false)
                if (error == null) installedDone(target)
                else {
                    progressText.text = getString(R.string.install_failed, error)
                    AlertDialog.Builder(this)
                        .setTitle(R.string.install_game)
                        .setMessage(getString(R.string.install_failed, error))
                        .setPositiveButton(R.string.close, null)
                        .show()
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        handler.removeCallbacksAndMessages(null)
        copying = false
        try { activeCopyStream?.close() } catch (_: Throwable) {}
        try {
            unregisterReceiver(tuDownloadDone)
        } catch (_: Throwable) {
        }
        // An install that outlives its screen would write into a folder
        // nobody is watching; it is stopped, and what was copied is kept
        // for next time.
        if (running && NativeBridge.isLoaded()) NativeBridge.nativeCancelInstall()
    }
}
