package com.droiddeck.launcher.core

import android.content.Context
import android.os.Build
import java.io.File

/**
 * Prebuilt helper executables (PulseAudio, the Vulkan bridge server) reference libc functions that
 * Android only has from 8.0. Before that, libddlegacylibc (cpp/compat/legacy_libc.c) supplies them
 * through LD_PRELOAD.
 */
object LegacyLibc {
    @JvmStatic
    fun preload(context: Context): String? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) return null
        return File(context.applicationInfo.nativeLibraryDir, "libddlegacylibc.so").takeIf { it.isFile }?.absolutePath
    }

    @JvmStatic
    fun environment(context: Context): Array<String> = preload(context)?.let { arrayOf("LD_PRELOAD=$it") } ?: emptyArray()
}
