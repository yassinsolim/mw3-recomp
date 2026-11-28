package com.mw2.recomp

import android.content.Context
import android.text.format.Formatter
import androidx.annotation.StringRes

/** The sentence that says why a disc image was refused: a string of the app and what it is formatted with. */
internal class DiscRefusalMessage(@StringRes val id: Int, val args: List<Any>)

/**
 * What the player is told for each kind of [DiscRefusal] (setup's disc step
 * and Repair / rebuild's), by the kind and nothing else: a disc of an
 * unsupported version says which version it found and which this app takes,
 * an image that cannot be read says whether it is gone or its permission is,
 * a full storage says how much is needed. [game] is the game's name, [label]
 * the image as the player knows it (its file name), [size] writes a number of bytes.
 */
internal fun DiscRefusal.message(game: String, label: String, size: (Long) -> String): DiscRefusalMessage = when (this) {
    is DiscRefusal.Unreadable -> when (reason) {
        DiscRefusal.Reason.MISSING -> DiscRefusalMessage(R.string.setup_disc_lost, listOf(label))
        DiscRefusal.Reason.PERMISSION -> DiscRefusalMessage(R.string.setup_disc_permission, listOf(label))
        DiscRefusal.Reason.NOT_SEEKABLE -> DiscRefusalMessage(R.string.setup_disc_not_on_device, listOf(game))
        DiscRefusal.Reason.IO -> DiscRefusalMessage(R.string.setup_disc_unreadable, listOf(label, cause?.message ?: cause.toString()))
    }
    is DiscRefusal.NoSpace -> DiscRefusalMessage(R.string.setup_disc_no_space, listOf(size(needed)))
    is DiscRefusal.NotThisGame -> DiscRefusalMessage(R.string.setup_disc_not_image, listOf(game))
    is DiscRefusal.UnsupportedVersion ->
        if (supported.isEmpty()) DiscRefusalMessage(R.string.setup_disc_version_found, listOf(game, found))
        else DiscRefusalMessage(R.string.setup_disc_version, listOf(game, found, supported.joinToString(" / ")))
    is DiscRefusal.Altered -> DiscRefusalMessage(R.string.setup_disc_altered, listOf(game, executable))
    is DiscRefusal.Failed -> DiscRefusalMessage(R.string.setup_disc_failed, listOf(message.orEmpty()))
}

/** [DiscRefusal.message] of the image [image], said in the app's language. */
internal fun DiscRefusal.text(context: Context, image: String): String {
    val said = message(Setup.gameName(context).orEmpty(), DiscImage.label(context, image).substringAfterLast('/')) { Formatter.formatFileSize(context, it) }
    return context.getString(said.id, *said.args.toTypedArray())
}
