package com.mw2.recomp

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.ConnectivityManager
import android.os.Build
import android.os.SystemClock
import androidx.core.content.IntentCompat
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Fetching the toolchain package (Toolchain.kt) and installing it, for a
 * build that finds none or one of another version (GameBuild.toolchain).
 *
 * The file is the release's that setup/toolchain.json names
 * (docs/android-releases.md): downloaded into the app's own cache, and given
 * to Android only when its SHA-256 is the one this app was built with.
 *
 * It is installed through a session of the system's installer, not by
 * opening the file: the app is then the package's installer of record, and
 * from Android 12 the installer of record may replace a package by a newer
 * one with no question to the player. So the first install asks once (a
 * package nobody installed before, or one that came by the older way of
 * opening the file), and every later toolchain is in place by the time the
 * build needs it. Where Android does ask, the question is [confirmation],
 * which a screen of this app shows ([ask]): a build in the background
 * cannot, and waits (GameBuild.Wait.CONFIRM).
 */
object ToolchainInstall {
    sealed class Result {
        object Installed : Result()

        /** Android asks the player; the answer comes later (ToolchainInstallReceiver). */
        object Asked : Result()

        /** Left to the player: a build they did not start neither installs a package they never had nor downloads over a metered network. */
        object Left : Result()

        class Failed(val message: String) : Result()
    }

    private val lock = Object()

    /** The session given to Android and not yet answered, -1 when there is none. */
    private var session = -1
    private var answer: Result? = null

    /** Builds waiting in [await]: an answer nobody waits for starts the build again. */
    private var waiting = 0

    /** Android's question about the session in hand, null when it asks nothing. */
    @Volatile
    var confirmation: Intent? = null
        private set

    /** The question was put to the player; it is not put twice. */
    private var asked = false

    /** Shows Android's question, once, from a screen of this app that is in front. */
    fun ask(activity: Activity) {
        val intent = synchronized(lock) {
            if (asked) return
            asked = true
            confirmation
        } ?: return
        try {
            activity.startActivity(intent)
        } catch (_: Exception) {
            // No installer screen on this device: the session ends unanswered
            // and the build says the package is not installed.
        }
    }

    /**
     * Shows Android's question again, when the player asks for it. Where
     * the app is not yet allowed to install packages the question sends
     * the player to Settings and closes; on the way back it does not come
     * up again by itself, and the session waits for an answer nobody can
     * give (seen on the AYN Thor, Android 13: "Waiting for your answer"
     * with nothing to answer).
     */
    fun askAgain(activity: Activity) {
        synchronized(lock) { asked = false }
        ask(activity)
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

    private fun metered(context: Context): Boolean =
        (context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).isActiveNetworkMetered

    /** The release's file into [target]; [progress] hears the megabytes done and their number. */
    private fun download(kit: GameBuild.Kit, target: File, progress: (Int, Int) -> Unit) {
        val partial = File(target.path + ".part")
        val connection = URL(BuildConfig.TOOLCHAIN_URL.ifEmpty { kit.toolchainDownload }).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = 20_000
            connection.readTimeout = 30_000
            if (connection.responseCode != HttpURLConnection.HTTP_OK) throw IOException("the server answered ${connection.responseCode}")
            val total = (connection.contentLengthLong shr 20).toInt()
            connection.inputStream.use { input ->
                partial.outputStream().use { output ->
                    val buffer = ByteArray(1 shl 16)
                    var done = 0L
                    while (true) {
                        val read = input.read(buffer)
                        if (read <= 0) break
                        output.write(buffer, 0, read)
                        done += read
                        progress((done shr 20).toInt(), total)
                    }
                }
            }
            if (!partial.renameTo(target)) throw IOException("the download could not be kept")
        } finally {
            connection.disconnect()
            partial.delete()
        }
    }

    /** Gives [file] to the system's installer as a new session; the answer comes to ToolchainInstallReceiver. */
    private fun commit(context: Context, file: File) {
        val installer = context.packageManager.packageInstaller
        // What a process the system ended left behind: never answered now.
        for (old in installer.mySessions) {
            try {
                installer.abandonSession(old.sessionId)
            } catch (_: Exception) {
            }
        }
        val parameters = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        parameters.setAppPackageName(Toolchain.PACKAGE)
        parameters.setSize(file.length())
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            parameters.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = installer.createSession(parameters)
        installer.openSession(id).use { opened ->
            opened.openWrite("toolchain", 0, file.length()).use { output ->
                file.inputStream().use { it.copyTo(output, 1 shl 16) }
                opened.fsync(output)
            }
            // The system fills the answer in, so the intent is one it may change.
            val mutable = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val answered = PendingIntent.getBroadcast(
                context, id, Intent(context, ToolchainInstallReceiver::class.java), PendingIntent.FLAG_UPDATE_CURRENT or mutable,
            )
            synchronized(lock) {
                session = id
                answer = null
                confirmation = null
                asked = false
            }
            opened.commit(answered.intentSender)
        }
    }

    /** Waits for Android's answer to the session in hand: at once where it asks nothing, never where it asks the player. */
    private fun await(): Result = synchronized(lock) {
        waiting++
        try {
            val until = SystemClock.elapsedRealtime() + ANSWER_MS
            while (answer == null && confirmation == null) {
                val left = until - SystemClock.elapsedRealtime()
                if (left <= 0) break
                lock.wait(left)
            }
            val result = answer
            when {
                result != null -> {
                    session = -1
                    answer = null
                    result
                }
                confirmation != null -> Result.Asked
                else -> {
                    session = -1
                    Result.Failed("Android did not answer the install")
                }
            }
        } finally {
            waiting--
        }
    }

    /**
     * Downloads the toolchain package this app builds with and installs it.
     * Blocks until it is installed, Android asks the player, or it failed.
     * [automatic]: the build was not started by the player. [progress] hears
     * the megabytes downloaded and their number.
     */
    fun run(context: Context, kit: GameBuild.Kit, automatic: Boolean, progress: (Int, Int) -> Unit): Result {
        // A session from an earlier run that Android has not answered, or has
        // answered since: its answer is this run's.
        if (synchronized(lock) { session != -1 }) return await()
        if (automatic && (!Toolchain.present(context) || metered(context))) return Result.Left
        val file = File(context.cacheDir, kit.toolchainAsset)
        try {
            // Kept from a download whose install the player was asked about
            // and this process did not live to hear the answer to.
            if (!(file.isFile && sha256(file) == kit.toolchainHash)) {
                download(kit, file, progress)
                // Anything else, a page of a proxy or a package of another
                // release, is deleted and never shown to the installer.
                if (sha256(file) != kit.toolchainHash) {
                    file.delete()
                    return Result.Failed(context.getString(R.string.build_toolchain_not_whole))
                }
            }
            commit(context, file)
        } catch (error: Exception) {
            return Result.Failed(error.message ?: error.toString())
        }
        val result = await()
        if (result is Result.Installed) file.delete()
        return result
    }

    /** Android's answer to the session, from ToolchainInstallReceiver. */
    internal fun answered(context: Context, intent: Intent) {
        val idle = synchronized(lock) {
            when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                    confirmation = IntentCompat.getParcelableExtra(intent, Intent.EXTRA_INTENT, Intent::class.java)
                    asked = false
                }
                PackageInstaller.STATUS_SUCCESS -> {
                    confirmation = null
                    answer = Result.Installed
                }
                else -> {
                    confirmation = null
                    answer = Result.Failed(
                        intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                            ?: context.getString(R.string.build_toolchain_missing),
                    )
                }
            }
            lock.notifyAll()
            answer != null && waiting == 0
        }
        // The player has answered a question the build stopped at: it
        // carries on, and says so itself if that answer was no.
        if (idle) GameBuildService.resumeIfInterrupted(context)
    }

    /** How long Android may take to install 45 MB when it asks nothing. */
    private const val ANSWER_MS = 3 * 60 * 1000L
}

/** Hears from the system's installer what became of a session ToolchainInstall committed. */
class ToolchainInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) = ToolchainInstall.answered(context.applicationContext, intent)
}
