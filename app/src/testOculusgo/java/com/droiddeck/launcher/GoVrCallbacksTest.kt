package com.droiddeck.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** The callbacks the native VR thread makes, on Android 7.1 (API 25). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class GoVrCallbacksTest {
    @Test fun roomCallbacksRunOnAndroid71() {
        val activity = Robolectric.buildActivity(OculusGoVrActivity::class.java).get()
        assertEquals(0, activity.onHomeFrame() and 1)
        val ui = activity.onUiPixels()
        assertNotNull(ui)
        assertEquals(1024 * 512 * 4, ui!!.capacity())
        activity.onPointer(1, 0.5f, 0.6f, true, false, false)
        activity.onPointer(0, 0f, 0f, false, false, false)
        activity.onKeyboardPixels()
        activity.onXrStatus()
    }
}
