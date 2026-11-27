package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.net.Uri

/** The project's Discord server (the address forwards to its invite): help, news and requests. Bugs still go to GitHub, with the log attached. */
object Discord {
    const val URL = "https://yassin.app/discord"

    /** Opens the server's link in the Discord app or the browser; nothing to do on a device that has neither. */
    fun open(activity: Activity) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(URL)))
        } catch (_: Exception) {
        }
    }
}
