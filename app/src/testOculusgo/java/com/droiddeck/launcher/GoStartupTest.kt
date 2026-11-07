package com.droiddeck.launcher

import android.content.Context
import com.droiddeck.launcher.session.OculusGo
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowBuild

/** Starts the Go app's process on Android 7.1 (API 25) so calls missing there fail here. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class GoStartupTest {
    @Test fun applicationStartsOnAndroid71() {
        ShadowBuild.setManufacturer("Oculus")
        ShadowBuild.setModel("Pacific")
        ShadowBuild.setDevice("pacific")
        val context: Context = RuntimeEnvironment.getApplication()
        OculusGo.initialize(context)
        assertTrue(OculusGo.isDevice())
    }
}
