package com.droiddeck.launcher.core

import android.app.Notification
import android.content.Context
import android.os.Build

/** Notification channels exist from Android 8.0 (API 26); earlier systems ignore the channel id. */
object Notifications {
    @JvmStatic
    fun builder(context: Context, channel: String): Notification.Builder =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) Notification.Builder(context, channel)
        else @Suppress("DEPRECATION") Notification.Builder(context)
}
