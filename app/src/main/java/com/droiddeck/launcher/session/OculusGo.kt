package com.droiddeck.launcher.session

import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import com.droiddeck.launcher.BuildConfig
import java.io.File

/**
 * Oculus Go (Snapdragon 821, Adreno 530, Android 7.1.2, 3 GB RAM).
 *
 * Turnip has no Adreno 5xx support and Venus needs Vulkan 1.2 from the phone driver, while the Go's
 * Adreno driver offers Vulkan 1.0. The session therefore takes the Vulkan bridge (MaliBridge), which
 * replays the guest's calls on the Go's own driver and emulates what 1.0 lacks, and falls back to
 * Mesa's lavapipe when the bridge's self-test fails.
 */
object OculusGo {
    private const val TAG = "OculusGo"
    const val LAVAPIPE_ICD = "/usr/share/vulkan/icd.d/lvp_icd.aarch64.json"
    const val VR_ACTIVITY = "com.droiddeck.launcher.OculusGoVrActivity"
    private const val VR_LIBRARY = "libdroiddeckgo.so"
    private const val DEFAULTS_KEY = "oculusGoDefaultsV1"

    internal fun matches(model: String, device: String, manufacturer: String): Boolean =
        manufacturer.equals("Oculus", ignoreCase = true) &&
            (model.equals("Pacific", ignoreCase = true) || device.equals("pacific", ignoreCase = true))

    @JvmStatic
    fun isDevice(): Boolean = matches(Build.MODEL, Build.DEVICE, Build.MANUFACTURER)

    fun immersiveAvailable(context: Context): Boolean =
        BuildConfig.OCULUS_GO && isDevice() && File(context.applicationInfo.nativeLibraryDir, VR_LIBRARY).isFile

    data class VrSettings(val refreshHz: Int, val guestDivisor: Int, val renderScalePercent: Int,
                          val cpuLevel: Int, val gpuLevel: Int)

    fun vrSettings(context: Context): VrSettings {
        val prefs = SessionPrefs.sessionPrefs(context)
        return VrSettings(
            refreshHz = prefs.getInt("oculusGo.refreshHz", 72).takeIf { it == 60 || it == 72 } ?: 72,
            guestDivisor = prefs.getInt("oculusGo.guestDivisor", 2).coerceIn(1, 4),
            renderScalePercent = prefs.getInt("oculusGo.renderScale", 75).coerceIn(40, 100),
            cpuLevel = prefs.getInt("oculusGo.cpuLevel", 4).coerceIn(0, 4),
            gpuLevel = prefs.getInt("oculusGo.gpuLevel", 4).coerceIn(0, 4),
        )
    }

    fun vrIntent(context: Context): Intent {
        val settings = vrSettings(context)
        return Intent().setClassName(context, VR_ACTIVITY)
            .putExtra("refresh_hz", settings.refreshHz)
            .putExtra("guest_divisor", settings.guestDivisor)
            .putExtra("render_scale_percent", settings.renderScalePercent)
            .putExtra("cpu_level", settings.cpuLevel)
            .putExtra("gpu_level", settings.gpuLevel)
    }

    /** Guest environment for a Go session; see the guest's droiddeck-session and droiddeck-steamvr. */
    fun guestEnv(): List<String> = listOf(
        "BL_OCULUS_GO=1",
        "BL_VKBRIDGE_FALLBACK_ICD=$LAVAPIPE_ICD",
        "BL_STEAM_SOFT_COMPOSITING=1",
        "LP_NUM_THREADS=3",
    )

    fun initialize(context: Context) {
        if (!isDevice()) return
        val prefs = SessionPrefs.sessionPrefs(context)
        if (!prefs.getBoolean(DEFAULTS_KEY, false)) {
            prefs.edit()
                .putBoolean("vkBridge", true)
                .putBoolean("venus", false)
                .putBoolean("vkBridgeSafe", false)
                .putString("linuxDriver", "")
                .putBoolean(DEFAULTS_KEY, true)
                .apply()
        }
        val report = buildString {
            appendLine("DroidDeck Oculus Go ${BuildConfig.VERSION_NAME}")
            appendLine("package=${context.packageName} goBuild=${BuildConfig.OCULUS_GO}")
            appendLine("manufacturer=${Build.MANUFACTURER} model=${Build.MODEL} device=${Build.DEVICE}")
            appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT} abis=${Build.SUPPORTED_ABIS.joinToString()}")
            for (name in listOf("gpu_model", "gpu_chipid")) {
                val value = runCatching { File("/sys/class/kgsl/kgsl-3d0/$name").readText().trim() }.getOrDefault("unavailable")
                appendLine("$name=$value")
            }
            appendLine("vulkan=Vulkan bridge on the system driver, lavapipe fallback")
            appendLine("immersiveSteamVr=${immersiveAvailable(context)} ${vrSettings(context)}")
        }
        Log.i(TAG, report)
        runCatching { File(context.filesDir, "oculusgo-startup.txt").writeText(report) }
            .onFailure { Log.w(TAG, "Could not write startup report", it) }
    }
}
