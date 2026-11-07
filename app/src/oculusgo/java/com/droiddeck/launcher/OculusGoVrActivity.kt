package com.droiddeck.launcher

import android.app.NativeActivity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import com.droiddeck.launcher.core.FileUtils
import com.droiddeck.launcher.gpu.SteamVr
import com.droiddeck.launcher.runtime.LinuxRuntime
import com.droiddeck.launcher.runtime.LinuxRuntimeInstaller
import com.droiddeck.launcher.runtime.RuntimeInstallService
import com.droiddeck.launcher.session.SessionPaths
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.wayland.CompositorHost
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The Oculus Go app is SteamVR only: it opens in a VR room (cpp/govr) whose panel installs the
 * runtime, starts the Steam session, shows Steam's screen for signing in, installs SteamVR and enters
 * it. The Go remote's laser drives the panel and touches Steam's screen. All callbacks except the
 * lifecycle ones arrive on the native VR thread.
 */
class OculusGoVrActivity : NativeActivity() {
    private enum class Phase { CHECKING, NEED_RUNTIME, INSTALLING_RUNTIME, STARTING_SESSION, PREPARING_STEAMVR, READY, ENTERING, IN_STEAMVR, FAILED }

    @Volatile private var phase = Phase.CHECKING
    @Volatile private var detail = ""
    @Volatile private var percent = -1
    @Volatile private var showScreen = true
    @Volatile private var immersive = false
    @Volatile private var steamVrStatus = SteamVr.Status.UNAVAILABLE
    @Volatile private var closing = false
    @Volatile private var uiDirty = true
    private var workerStarted = false

    private val ui = Bitmap.createBitmap(UI_WIDTH, UI_HEIGHT, Bitmap.Config.ARGB_8888)
    private val uiPixels: ByteBuffer = ByteBuffer.allocateDirect(UI_WIDTH * UI_HEIGHT * 4).order(ByteOrder.nativeOrder())
    private var screenTexture: SurfaceTexture? = null
    private var screenSurface: Surface? = null
    @Volatile private var screenFrame = false
    private val screenMatrix = FloatArray(16)
    private val keyboard by lazy { VrKeyboard(getString(R.string.go_vr_space)) }
    @Volatile private var keyboardVisible = false
    private var lastKey = false
    private var touching = false
    private var lastClick = false
    private var lastBack = false

    override fun onCreate(savedInstanceState: Bundle?) {
        getExternalFilesDir(null)
        super.onCreate(savedInstanceState)
        if (!workerStarted) {
            workerStarted = true
            Thread({ prepare() }, "go-vr-home").start()
        }
    }

    override fun onResume() {
        super.onResume()
        SessionService.setXrActivityVisible(this, true)
        if (SessionState.running) SessionService.resume(this)
    }

    override fun onPause() {
        SessionService.setXrActivityVisible(this, false)
        super.onPause()
    }

    override fun onDestroy() {
        closing = true
        if (SessionState.running) SessionService.stop(this)
        CompositorHost.detach(screenSurface)
        screenSurface?.release()
        screenTexture?.release()
        super.onDestroy()
    }

    private fun set(newPhase: Phase, text: String = "", progress: Int = -1) {
        phase = newPhase
        detail = text
        percent = progress
        uiDirty = true
    }

    /** Runtime, then the Steam session, then SteamVR; the panel follows each step. */
    private fun prepare() {
        try {
            if (!LinuxRuntime.isInstalled(this)) {
                set(Phase.NEED_RUNTIME, getString(R.string.go_vr_runtime_needed))
                return
            }
            startSession()
        } catch (e: Exception) {
            Log.e(TAG, "home preparation failed", e)
            set(Phase.FAILED, e.message ?: e.toString())
        }
    }

    private fun installRuntime() {
        set(Phase.INSTALLING_RUNTIME, getString(R.string.go_vr_downloading), 0)
        Thread({
            try {
                val release = LinuxRuntimeInstaller.fetchRelease() ?: throw IllegalStateException(getString(R.string.session_runtime_catalog_unreachable))
                runCatching { RuntimeInstallService.start(this, release) }
                val ok = LinuxRuntimeInstaller.install(this, release) { stage, progress ->
                    set(Phase.INSTALLING_RUNTIME, stage, progress)
                }
                if (!ok) throw IllegalStateException(getString(R.string.session_runtime_install_failed))
                startSession()
            } catch (e: Exception) {
                Log.e(TAG, "runtime install failed", e)
                set(Phase.FAILED, e.message ?: e.toString())
            }
        }, "go-vr-runtime").start()
    }

    private fun startSession() {
        set(Phase.STARTING_SESSION, getString(R.string.go_vr_starting_steam))
        val surface = waitForScreenSurface()
        val runtimeDir = File(filesDir, ".wayland-rt").apply { mkdirs() }
        if (!CompositorHost.isStarted) {
            FileUtils.copyAsset(this, "wayland/keymap.xkb", File(runtimeDir, "keymap.xkb"))
            val waylandLog = File(SessionPaths.beginOrCurrent(this), "wayland.log")
            WaylandCompositor.setSessionLogFile(waylandLog)
            runCatching { android.system.Os.setenv("BL_WAYLAND_LOG", waylandLog.path, true) }
            WaylandCompositor.nativeSetScaleMode(SCALE_FIT, ALIGN_CENTER)
        }
        SessionState.outputSize = SCREEN_WIDTH to SCREEN_HEIGHT
        CompositorHost.startOrAttach(this, surface, runtimeDir.path, null, null, applicationInfo.nativeLibraryDir,
            SCREEN_WIDTH, SCREEN_HEIGHT, 60f, 0)
        if (!SessionState.running) SessionService.start(this, SessionService.MODE_STEAM)
        watchSteamVr()
    }

    private fun waitForScreenSurface(): Surface {
        val deadline = SystemClock.elapsedRealtime() + 30_000
        while (screenSurface == null) {
            check(!closing && SystemClock.elapsedRealtime() < deadline) { "VR screen surface unavailable" }
            Thread.sleep(50)
        }
        return screenSurface!!
    }

    private fun watchSteamVr() {
        var installTried = false
        val started = SystemClock.elapsedRealtime()
        while (!closing) {
            if (phase == Phase.ENTERING || phase == Phase.IN_STEAMVR) {
                Thread.sleep(1000)
                continue
            }
            if (!SessionState.running && SessionState.stopRequested) {
                set(Phase.FAILED, getString(R.string.go_vr_session_ended))
                return
            }
            val status = if (SessionState.running) SteamVr.status(this) else SteamVr.Status.UNAVAILABLE
            steamVrStatus = status
            when (status) {
                SteamVr.Status.NOT_INSTALLED -> if (!installTried) {
                    installTried = true
                    set(Phase.PREPARING_STEAMVR, getString(R.string.go_vr_installing_steamvr))
                    SteamVr.run(this, "install")
                }
                SteamVr.Status.INSTALLING -> set(Phase.PREPARING_STEAMVR, getString(R.string.go_vr_installing_steamvr))
                SteamVr.Status.READY, SteamVr.Status.RUNNING -> if (phase != Phase.READY) set(Phase.READY, getString(R.string.go_vr_ready))
                SteamVr.Status.UNAVAILABLE -> {
                    val waited = (SystemClock.elapsedRealtime() - started) / 1000
                    set(Phase.STARTING_SESSION, getString(R.string.go_vr_starting_steam) + " (${waited}s)")
                }
            }
            Thread.sleep(2000)
        }
    }

    private fun enterSteamVr() {
        if (phase != Phase.READY) return
        set(Phase.ENTERING, getString(R.string.go_vr_entering))
        immersive = true
    }

    private fun leaveSteamVr(reason: String?) {
        Log.w(TAG, "back to the VR room: ${reason ?: "SteamVR closed"}")
        immersive = false
        set(if (reason == null) Phase.READY else Phase.FAILED, reason ?: getString(R.string.go_vr_ready))
    }

    fun onHomeFrame(): Int {
        val screen = showScreen && SessionState.running
        return (if (immersive) 1 else 0) or (if (screen) 2 else 0) or (if (screen && keyboardVisible) 4 else 0)
    }

    fun onKeyboardPixels(): ByteBuffer? = if (keyboardVisible) keyboard.pixels() else null

    fun onUiPixels(): ByteBuffer? {
        if (!uiDirty) return null
        uiDirty = false
        drawPanel()
        uiPixels.rewind()
        ui.copyPixelsToBuffer(uiPixels)
        uiPixels.rewind()
        return uiPixels
    }

    fun onScreenTexture(texture: Int): FloatArray? {
        val surfaceTexture = screenTexture ?: SurfaceTexture(texture).also { created ->
            created.setDefaultBufferSize(SCREEN_WIDTH, SCREEN_HEIGHT)
            created.setOnFrameAvailableListener { screenFrame = true }
            screenTexture = created
            screenSurface = Surface(created)
        }
        if (!screenFrame && !hasScreenImage) return null
        if (screenFrame) {
            screenFrame = false
            surfaceTexture.updateTexImage()
            surfaceTexture.getTransformMatrix(screenMatrix)
            hasScreenImage = true
        }
        return screenMatrix
    }
    private var hasScreenImage = false

    fun onPointer(target: Int, u: Float, v: Float, trigger: Boolean, click: Boolean, back: Boolean) {
        if (back && !lastBack && SessionState.running) {
            showScreen = !showScreen
            uiDirty = true
        }
        lastBack = back
        val x = (u * SCREEN_WIDTH).toInt().coerceIn(0, SCREEN_WIDTH - 1)
        val y = (v * SCREEN_HEIGHT).toInt().coerceIn(0, SCREEN_HEIGHT - 1)
        val pressed = target == TARGET_SCREEN && (trigger || click)
        when {
            pressed && !touching -> { WaylandCompositor.nativeSendTouch(0, TOUCH_ID, x, y); touching = true }
            pressed -> WaylandCompositor.nativeSendTouch(1, TOUCH_ID, x, y)
            touching -> { WaylandCompositor.nativeSendTouch(2, TOUCH_ID, x, y); touching = false }
        }
        val press = target == TARGET_UI && (trigger || click)
        if (press && !lastClick) clickPanel(u, v)
        lastClick = press
        val key = target == TARGET_KEYBOARD && (trigger || click)
        if (key && !lastKey && keyboard.press(u, v)) {
            keyboardVisible = false
            uiDirty = true
        }
        if (!key && lastKey) keyboard.release()
        lastKey = key
    }

    private fun clickPanel(u: Float, v: Float) {
        if (u !in BUTTON.left..BUTTON.right || v !in BUTTON.top..BUTTON.bottom) {
            if (SessionState.running && u in TOGGLE.left..TOGGLE.right && v in TOGGLE.top..TOGGLE.bottom) {
                showScreen = !showScreen
                uiDirty = true
            } else if (SessionState.running && showScreen && u in KEYBOARD.left..KEYBOARD.right && v in KEYBOARD.top..KEYBOARD.bottom) {
                keyboardVisible = true
            }
            return
        }
        when (phase) {
            Phase.NEED_RUNTIME -> installRuntime()
            Phase.READY -> enterSteamVr()
            Phase.FAILED -> {
                set(Phase.CHECKING)
                Thread({ prepare() }, "go-vr-home").start()
            }
            else -> {}
        }
    }

    private fun drawPanel() {
        val canvas = Canvas(ui)
        canvas.drawColor(Color.argb(235, 18, 22, 32))
        val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = 56f; isFakeBoldText = true }
        canvas.drawText("DroidDeck Go", 48f, 92f, text)
        text.apply { textSize = 34f; isFakeBoldText = false; color = Color.rgb(190, 200, 215) }
        canvas.drawText(detail.take(52), 48f, 160f, text)
        if (percent in 0..100) {
            val bar = Paint().apply { color = Color.rgb(45, 52, 66) }
            canvas.drawRoundRect(RectF(48f, 190f, UI_WIDTH - 48f, 210f), 10f, 10f, bar)
            bar.color = Color.rgb(64, 156, 255)
            canvas.drawRoundRect(RectF(48f, 190f, 48f + (UI_WIDTH - 96f) * percent / 100f, 210f), 10f, 10f, bar)
        }
        val label = when (phase) {
            Phase.NEED_RUNTIME -> getString(R.string.go_vr_install)
            Phase.READY -> getString(R.string.go_vr_enter)
            Phase.FAILED -> getString(R.string.go_vr_retry)
            Phase.ENTERING, Phase.IN_STEAMVR -> getString(R.string.go_vr_entering)
            else -> getString(R.string.go_vr_wait)
        }
        val active = phase == Phase.NEED_RUNTIME || phase == Phase.READY || phase == Phase.FAILED
        drawButton(canvas, BUTTON, label, if (active) Color.rgb(64, 156, 255) else Color.rgb(60, 66, 80), 46f)
        if (SessionState.running) {
            drawButton(canvas, TOGGLE, getString(if (showScreen) R.string.go_vr_hide_screen else R.string.go_vr_show_screen),
                Color.rgb(60, 66, 80), 30f)
            if (showScreen) drawButton(canvas, KEYBOARD, getString(R.string.go_vr_keyboard), Color.rgb(60, 66, 80), 30f)
        }
    }

    private fun drawButton(canvas: Canvas, area: RectF, label: String, fill: Int, size: Float) {
        val rect = RectF(area.left * UI_WIDTH, area.top * UI_HEIGHT, area.right * UI_WIDTH, area.bottom * UI_HEIGHT)
        canvas.drawRoundRect(rect, 24f, 24f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = fill })
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE; textSize = size; textAlign = Paint.Align.CENTER; isFakeBoldText = true
        }
        canvas.drawText(label, rect.centerX(), rect.centerY() - (paint.descent() + paint.ascent()) / 2, paint)
    }

    fun onXrActive(active: Boolean) {
        if (active) {
            check(CompositorHost.isStarted) { "DroidDeck compositor is not running" }
            SessionService.resume(this)
            WaylandCompositor.nativeSetCardboard(false, 1, 0.064f, 3f, 4f, 0f, 0f, 90f, 0f, 0f, false, 0f)
            WaylandCompositor.nativeSetFrameGenArmed(false, 0, 0)
            WaylandCompositor.nativeSetZeroCopy(false)
            CompositorHost.attachXr()
            WaylandCompositor.nativeVsync(System.nanoTime())
            check(WaylandCompositor.nativeWaitSurface(3000)) { "XR frame GPU handshake timed out" }
            phase = Phase.IN_STEAMVR
            startSteamVr()
        } else {
            screenSurface?.let { surface ->
                if (CompositorHost.isStarted) CompositorHost.startOrAttach(this, surface, File(filesDir, ".wayland-rt").path,
                    null, null, applicationInfo.nativeLibraryDir, SCREEN_WIDTH, SCREEN_HEIGHT, 60f, 0)
            }
        }
    }

    private fun startSteamVr() {
        Thread({
            val session = SessionState.sessionId
            var started = false
            try {
                started = SteamVr.run(this, "start")
                if (!started) {
                    leaveSteamVr(getString(R.string.cardboard_steamvr_failed))
                    return@Thread
                }
                val startupDeadline = SystemClock.elapsedRealtime() + 60_000
                var lastRunning: Long? = null
                while (!closing && immersive) {
                    if (!SessionState.running || SessionState.sessionId != session) {
                        leaveSteamVr(getString(R.string.go_vr_session_ended))
                        break
                    }
                    steamVrStatus = SteamVr.status(this)
                    val now = SystemClock.elapsedRealtime()
                    if (steamVrStatus == SteamVr.Status.RUNNING) lastRunning = now
                    if (steamVrStatus != SteamVr.Status.RUNNING && now >= (lastRunning?.plus(5_000) ?: startupDeadline)) {
                        leaveSteamVr(null)
                        break
                    }
                    Thread.sleep(1000)
                }
            } finally {
                if (started && SessionState.running) SteamVr.run(this, "stop")
            }
        }, "go-steamvr").start()
    }

    fun onXrFrame(frameTimeNanos: Long) {
        WaylandCompositor.nativeVsync(frameTimeNanos)
    }

    fun onXrStatus(): Int = steamVrStatus.ordinal

    companion object {
        private const val TAG = "OculusGoVr"
        private const val UI_WIDTH = 1024
        private const val UI_HEIGHT = 512
        private const val SCREEN_WIDTH = 1280
        private const val SCREEN_HEIGHT = 720
        private const val TARGET_UI = 1
        private const val TARGET_SCREEN = 2
        private const val TARGET_KEYBOARD = 3
        private const val TOUCH_ID = 9
        private const val SCALE_FIT = 1
        private const val ALIGN_CENTER = 0
        private val BUTTON = RectF(0.08f, 0.50f, 0.92f, 0.76f)
        private val TOGGLE = RectF(0.08f, 0.81f, 0.49f, 0.95f)
        private val KEYBOARD = RectF(0.51f, 0.81f, 0.92f, 0.95f)
    }
}
