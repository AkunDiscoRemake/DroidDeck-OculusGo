package com.droiddeck.launcher.core

import android.os.Build
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.util.ReflectionHelpers
import java.io.File
import java.util.concurrent.TimeUnit

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ProcessCompatTest {
    private fun legacy() = ReflectionHelpers.setStaticField(Build.VERSION::class.java, "SDK_INT", 25)

    @After fun restore() = ReflectionHelpers.setStaticField(Build.VERSION::class.java, "SDK_INT", 28)

    @Test fun legacyWaitAndKill() {
        legacy()
        val sleeper = ProcessBuilder("sleep", "5").start()
        assertTrue(sleeper.aliveCompat)
        assertFalse(sleeper.waitForCompat(50, TimeUnit.MILLISECONDS))
        sleeper.destroyForciblyCompat()
        assertTrue(sleeper.waitForCompat(5, TimeUnit.SECONDS))
        assertFalse(sleeper.aliveCompat)
    }

    @Test fun legacyRedirectAppendsOutput() {
        legacy()
        val file = File.createTempFile("process-compat", ".log")
        file.writeText("first\n")
        val process = ProcessCompat.startWithOutput(ProcessBuilder("echo", "second").redirectErrorStream(true), file, true)
        assertTrue(process.waitForCompat(5, TimeUnit.SECONDS))
        val deadline = System.currentTimeMillis() + 5000
        while (file.readText() != "first\nsecond\n" && System.currentTimeMillis() < deadline) Thread.sleep(10)
        assertEquals("first\nsecond\n", file.readText())
        file.delete()
    }

    @Test fun currentRedirectTruncates() {
        val file = File.createTempFile("process-compat", ".log")
        file.writeText("old\n")
        val process = ProcessCompat.startWithOutput(ProcessBuilder("echo", "new"), file, false)
        assertTrue(process.waitForCompat(5, TimeUnit.SECONDS))
        assertEquals("new\n", file.readText())
        file.delete()
    }
}
