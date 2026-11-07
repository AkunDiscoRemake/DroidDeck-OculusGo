package com.droiddeck.launcher.core

import android.os.Build
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.TimeUnit

/** Process and redirect calls that Android only has from 8.0 (API 26); Oculus Go runs 7.1. */
object ProcessCompat {
    @JvmStatic
    fun isAlive(process: Process): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) return process.isAlive
        return try {
            process.exitValue()
            false
        } catch (e: IllegalThreadStateException) {
            true
        }
    }

    @JvmStatic
    @Throws(InterruptedException::class)
    fun waitFor(process: Process, timeout: Long, unit: TimeUnit): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) return process.waitFor(timeout, unit)
        val deadline = System.nanoTime() + unit.toNanos(timeout)
        while (isAlive(process)) {
            val left = deadline - System.nanoTime()
            if (left <= 0) return false
            Thread.sleep(TimeUnit.NANOSECONDS.toMillis(left).coerceIn(1, 20))
        }
        return true
    }

    /** Android's Process.destroy() already sends SIGKILL, which is what destroyForcibly() adds. */
    @JvmStatic
    fun destroyForcibly(process: Process): Process {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) return process.destroyForcibly()
        process.destroy()
        return process
    }

    /**
     * Starts [builder] with its standard output (and error, if merged) going to [output], appended
     * or truncated; null discards it. Before API 26 the output is copied by a daemon thread.
     */
    @JvmStatic
    fun startWithOutput(builder: ProcessBuilder, output: File?, append: Boolean): Process {
        val target = output ?: File("/dev/null")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder.redirectOutput(if (append) ProcessBuilder.Redirect.appendTo(target) else ProcessBuilder.Redirect.to(target))
            return builder.start()
        }
        val process = builder.start()
        val sink: OutputStream = if (output == null) NullSink else FileOutputStream(target, append)
        pump(process.inputStream, sink, "proc-redirect")
        return process
    }

    private fun pump(from: InputStream, to: OutputStream, name: String) =
        Thread({
            val buffer = ByteArray(16384)
            try {
                while (true) {
                    val n = from.read(buffer)
                    if (n < 0) break
                    to.write(buffer, 0, n)
                }
            } catch (e: java.io.IOException) {
            } finally {
                runCatching { to.close() }
            }
        }, name).apply { isDaemon = true; start() }

    private object NullSink : OutputStream() {
        override fun write(b: Int) {}
        override fun write(b: ByteArray, off: Int, len: Int) {}
    }
}

val Process.aliveCompat: Boolean get() = ProcessCompat.isAlive(this)

fun Process.waitForCompat(timeout: Long, unit: TimeUnit): Boolean = ProcessCompat.waitFor(this, timeout, unit)

fun Process.destroyForciblyCompat(): Process = ProcessCompat.destroyForcibly(this)
