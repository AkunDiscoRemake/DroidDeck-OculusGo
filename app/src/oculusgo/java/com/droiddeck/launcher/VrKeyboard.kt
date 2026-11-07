package com.droiddeck.launcher

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import com.droiddeck.launcher.agent.AgentInput
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A US QWERTY keyboard drawn on a VR panel; presses go to the session as evdev key events. */
class VrKeyboard(private val spaceLabel: String) {
    private sealed interface Action {
        data class Type(val char: Char) : Action
        data class Key(val evdev: Int) : Action
        object Shift : Action
        object Symbols : Action
        object Close : Action
    }

    private class Key(val label: String, val action: Action, val rect: RectF)

    private var shift = false
    private var symbols = false
    private var pressed: Key? = null
    private var keys = layout()
    @Volatile private var dirty = true
    private val bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888)
    private val pixels: ByteBuffer = ByteBuffer.allocateDirect(WIDTH * HEIGHT * 4).order(ByteOrder.nativeOrder())

    /** True when the press asks to close the keyboard. */
    fun press(u: Float, v: Float): Boolean {
        val key = keys.firstOrNull { u in it.rect.left..it.rect.right && v in it.rect.top..it.rect.bottom } ?: return false
        pressed = key
        dirty = true
        when (val action = key.action) {
            is Action.Type -> {
                val char = if (shift && action.char.isLetter()) action.char.uppercaseChar() else action.char
                type(char)
                if (shift) { shift = false; keys = layout() }
            }
            is Action.Key -> tap(action.evdev, false)
            Action.Shift -> { shift = !shift; keys = layout() }
            Action.Symbols -> { symbols = !symbols; shift = false; keys = layout() }
            Action.Close -> return true
        }
        return false
    }

    fun release() {
        if (pressed != null) dirty = true
        pressed = null
    }

    fun pixels(): ByteBuffer? {
        if (!dirty) return null
        dirty = false
        draw()
        pixels.rewind()
        bitmap.copyPixelsToBuffer(pixels)
        pixels.rewind()
        return pixels
    }

    private fun type(char: Char) {
        val (evdev, shifted) = AgentInput.TYPED[char] ?: return
        tap(evdev, shifted)
    }

    private fun tap(evdev: Int, shifted: Boolean) {
        if (shifted) WaylandCompositor.nativeSendKey(LEFT_SHIFT, 1)
        WaylandCompositor.nativeSendKey(evdev, 1)
        WaylandCompositor.nativeSendKey(evdev, 0)
        if (shifted) WaylandCompositor.nativeSendKey(LEFT_SHIFT, 0)
    }

    private fun layout(): List<Key> {
        val rows = if (symbols) listOf("!@#\$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/", "`~?") else
            listOf("1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm")
        val result = ArrayList<Key>()
        val rowHeight = 1f / 5f
        fun add(label: String, action: Action, row: Int, start: Float, width: Float) {
            result += Key(label, action, RectF(start + GAP, row * rowHeight + GAP, start + width - GAP, (row + 1) * rowHeight - GAP))
        }
        for ((row, chars) in rows.withIndex()) {
            val unit = 1f / 10f
            if (row < 3) {
                val start = (1f - chars.length * unit) / 2f
                chars.forEachIndexed { i, c -> add(c.toString(), Action.Type(c), row, start + i * unit, unit) }
            } else {
                add("↑", Action.Shift, row, 0f, 1.5f * unit)
                val start = 1.5f * unit + (7f - chars.length) * unit / 2f
                chars.forEachIndexed { i, c -> add(c.toString(), Action.Type(c), row, start + i * unit, unit) }
                add("←", Action.Key(BACKSPACE), row, 8.5f * unit, 1.5f * unit)
            }
        }
        add(if (symbols) "ABC" else "?123", Action.Symbols, 4, 0f, 0.15f)
        add("@", Action.Type('@'), 4, 0.15f, 0.1f)
        add(spaceLabel, Action.Type(' '), 4, 0.25f, 0.4f)
        add(".", Action.Type('.'), 4, 0.65f, 0.1f)
        add("Enter", Action.Key(ENTER), 4, 0.75f, 0.15f)
        add("×", Action.Close, 4, 0.9f, 0.1f)
        return result
    }

    private fun draw() {
        val canvas = Canvas(bitmap)
        canvas.drawColor(Color.argb(235, 18, 22, 32))
        val fill = Paint(Paint.ANTI_ALIAS_FLAG)
        val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textAlign = Paint.Align.CENTER; textSize = 40f }
        for (key in keys) {
            val rect = RectF(key.rect.left * WIDTH, key.rect.top * HEIGHT, key.rect.right * WIDTH, key.rect.bottom * HEIGHT)
            fill.color = when {
                key === pressed -> Color.rgb(64, 156, 255)
                key.action == Action.Shift && shift -> Color.rgb(64, 156, 255)
                key.action is Action.Type -> Color.rgb(52, 58, 72)
                else -> Color.rgb(38, 43, 55)
            }
            canvas.drawRoundRect(rect, 12f, 12f, fill)
            val label = if (shift && key.label.length == 1 && key.label[0].isLetter()) key.label.uppercase() else key.label
            text.textSize = if (label.length > 2) 30f else 40f
            canvas.drawText(label, rect.centerX(), rect.centerY() - (text.descent() + text.ascent()) / 2, text)
        }
    }

    companion object {
        const val WIDTH = 1024
        const val HEIGHT = 384
        private const val GAP = 0.006f
        private const val LEFT_SHIFT = 42
        private const val BACKSPACE = 14
        private const val ENTER = 28
    }
}
