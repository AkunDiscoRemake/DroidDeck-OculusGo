package com.droiddeck.launcher.audio

import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Process
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileInputStream

/**
 * PulseAudio output for Android before 8.0 (Oculus Go): the DirectAudio and AAudio sinks need
 * AAudio (API 26), so PulseAudio's module-pipe-sink writes 48 kHz stereo s16le into a FIFO and this
 * thread plays it through AudioTrack.
 */
class LegacyAudioOut(val fifo: File) {
    @Volatile private var running = false
    private var thread: Thread? = null

    fun sinkConfig(): List<String> = listOf(
        "load-module module-pipe-sink sink_name=$SINK file=\"${fifo.absolutePath}\" format=s16le rate=$RATE channels=2",
        "set-default-sink $SINK",
    )

    fun start() {
        stop()
        fifo.delete()
        runCatching { Os.mkfifo(fifo.absolutePath, 0x180) }.onFailure { Log.w(TAG, "mkfifo $fifo", it) }
        running = true
        thread = Thread({ play() }, "legacy-audio-out").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread = null
    }

    private fun play() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val minimum = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val bytes = maxOf(minimum, CHUNK * 2)
        @Suppress("DEPRECATION")
        val track = AudioTrack(AudioManager.STREAM_MUSIC, RATE, AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_16BIT, bytes, AudioTrack.MODE_STREAM)
        try {
            track.play()
            val buffer = ByteArray(CHUNK)
            while (running) {
                FileInputStream(fifo).use { input ->
                    runCatching { Os.fcntlInt(input.fd, F_SETPIPE_SZ, PIPE_BYTES) }
                    var held = 0
                    while (running) {
                        val n = input.read(buffer, held, buffer.size - held)
                        if (n < 0) break
                        val total = held + n
                        val whole = total - total % FRAME_BYTES
                        if (whole > 0) track.write(buffer, 0, whole)
                        held = total - whole
                        if (held > 0) System.arraycopy(buffer, whole, buffer, 0, held)
                    }
                }
            }
        } catch (e: Exception) {
            if (running) Log.w(TAG, "audio output stopped", e)
        } finally {
            runCatching { track.stop() }
            track.release()
        }
    }

    companion object {
        private const val TAG = "LegacyAudioOut"
        const val SINK = "AudioTrackOut"
        private const val RATE = 48000
        private const val CHUNK = 3840
        private const val FRAME_BYTES = 4
        /** Linux F_SETPIPE_SZ; 8 KiB keeps PulseAudio's write-ahead near 40 ms. */
        private const val F_SETPIPE_SZ = 1031
        private const val PIPE_BYTES = 8192
    }
}
