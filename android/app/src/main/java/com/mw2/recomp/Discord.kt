package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.net.Uri

/** The project's Discord server: help, news and requests. Bugs still go to GitHub, with the log attached. */
object Discord {
    const val INVITE = "https://discord.gg/bTKn3Su3sp"

    /** Opens the invite in the Discord app or the browser; nothing to do on a device that has neither. */
    fun open(activity: Activity) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(INVITE)))
        } catch (_: Exception) {
        }
    }
}
