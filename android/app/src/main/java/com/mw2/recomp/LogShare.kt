package com.mw2.recomp

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import androidx.appcompat.app.AlertDialog
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The log as a player hands it to somebody: a text file, shared through the
 * system's share list, which is what a bug report's form takes as an
 * attachment (.github/ISSUE_TEMPLATE/bug-report.yml).
 *
 * It went as the share's text before, up to 200,000 characters of it: GitHub
 * takes 65,536 in an issue's body, so the log could not be pasted, and with
 * no file there was nothing to attach. The file is written to the app's
 * cache and read by the app the player picks through a FileProvider
 * (res/xml/log_paths.xml), with leave for that one file only.
 */
object LogShare {
    /** The app's version as its name says it: "0.1.9", without the commit after it. */
    private fun version(context: Context): String = try {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty().substringBefore(' ')
    } catch (_: Exception) {
        ""
    }

    /** This game in a file's name and as the report form's list has it, by the title's number. */
    private val game: Pair<String, String> = when {
        Title.isAw -> "aw" to "AW (Advanced Warfare)"
        Title.isCod4 -> "cod4" to "CoD4 (Call of Duty 4)"
        Title.isMw3 -> "mw3" to "MW3 (Modern Warfare 3)"
        else -> "mw2" to "MW2 (Modern Warfare 2)"
    }

    /** Writes [text] as <game>-log-<version>-<date>.txt, the only file kept, and returns its address for another app. */
    private fun file(context: Context, text: String): Uri {
        val folder = File(context.cacheDir, "shared-logs")
        folder.deleteRecursively()
        folder.mkdirs()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(Date())
        val file = File(folder, "${game.first}-log-${version(context)}-$stamp.txt")
        file.writeText(text)
        return FileProvider.getUriForFile(context, "${context.packageName}.logs", file)
    }

    /** Shares [text] as a file, or copies it for an app that only takes text: the player chooses. */
    fun share(activity: Activity, text: String) {
        AlertDialog.Builder(activity)
            .setTitle(R.string.share)
            .setItems(arrayOf(activity.getString(R.string.share_log_file), activity.getString(R.string.share_log_copy))) { _, which ->
                if (which == 0) send(activity, text) else copy(activity, text)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun send(activity: Activity, text: String) {
        val uri = try {
            file(activity, text)
        } catch (_: Exception) {
            // No room in the cache: the text is what can still be handed over.
            copy(activity, text)
            return
        }
        val intent = Intent(Intent.ACTION_SEND).setType("text/plain")
            .putExtra(Intent.EXTRA_STREAM, uri)
            .putExtra(Intent.EXTRA_SUBJECT, uri.lastPathSegment)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        // The share list shows the file by this, and is given leave to read it through it.
        intent.clipData = ClipData.newRawUri(uri.lastPathSegment, uri)
        activity.startActivity(Intent.createChooser(intent, activity.getString(R.string.share_log_file)))
    }

    /**
     * The end of [text] on the clipboard, for a chat app: the end is the part
     * anyone reads, and a clipboard takes far less than a file does.
     */
    private fun copy(context: Context, text: String) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText(context.getString(R.string.view_log), text.takeLast(COPIED)))
        android.widget.Toast.makeText(context, R.string.share_log_copied, android.widget.Toast.LENGTH_SHORT).show()
    }

    /**
     * "Report a problem": the project's bug report form in the browser, with
     * the game, the app's version and the device filled in, after saying
     * that the log's file is to be attached to it and where it comes from.
     */
    fun report(activity: Activity, wholeLog: () -> String) {
        val form = Uri.parse("https://github.com/${BuildConfig.UPDATE_REPO.ifEmpty { "yassinsolim/mw3-recomp" }}/issues/new").buildUpon()
            .appendQueryParameter("template", "bug-report.yml")
            .appendQueryParameter("game", game.second)
            .appendQueryParameter("version", version(activity))
            .appendQueryParameter("device", "${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE}")
            .build()
        AlertDialog.Builder(activity)
            .setTitle(R.string.report_problem_title)
            .setMessage(R.string.report_problem_explained)
            .setPositiveButton(R.string.report_problem_open) { _, _ ->
                try {
                    activity.startActivity(Intent(Intent.ACTION_VIEW, form))
                } catch (_: Exception) {
                    // No browser on this device: nothing to open the form in.
                }
            }
            .setNeutralButton(R.string.share_log_file) { _, _ -> send(activity, wholeLog()) }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /** What a chat app's message takes without cutting it, and well under what a clipboard holds. */
    private const val COPIED = 30_000
}
