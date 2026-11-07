package com.droiddeck.launcher.session

import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowBuild

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class OculusGoTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()

    @Before fun reset() {
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit().clear().commit()
        ShadowBuild.setManufacturer("Google")
        ShadowBuild.setModel("Phone")
        ShadowBuild.setDevice("phone")
    }

    @Test fun recognisesOnlyTheGo() {
        assertTrue(OculusGo.matches("Pacific", "pacific", "Oculus"))
        assertTrue(OculusGo.matches("Oculus Go", "pacific", "oculus"))
        assertFalse(OculusGo.matches("Quest 2", "hollywood", "Oculus"))
        assertFalse(OculusGo.matches("Pacific", "pacific", "Google"))
    }

    @Test fun otherDevicesKeepTheirSettings() {
        OculusGo.initialize(context)
        val prefs = SessionPrefs.sessionPrefs(context)
        assertFalse(prefs.contains("oculusGoDefaultsV1"))
        assertFalse(prefs.contains("vkBridge"))
    }

    @Test fun goDefaultsToTheVulkanBridgeOnce() {
        ShadowBuild.setManufacturer("Oculus")
        ShadowBuild.setModel("Pacific")
        ShadowBuild.setDevice("pacific")
        OculusGo.initialize(context)
        assertTrue(SessionPrefs.vkBridge(context))
        assertFalse(SessionPrefs.venus(context))
        SessionPrefs.sessionPrefs(context).edit().putBoolean("vkBridge", false).commit()
        OculusGo.initialize(context)
        assertFalse(SessionPrefs.vkBridge(context))
    }

    @Test fun vrSettingsAreClamped() {
        SessionPrefs.sessionPrefs(context).edit()
            .putInt("oculusGo.refreshHz", 90).putInt("oculusGo.guestDivisor", 9)
            .putInt("oculusGo.renderScale", 10).putInt("oculusGo.cpuLevel", 7).commit()
        val settings = OculusGo.vrSettings(context)
        assertEquals(72, settings.refreshHz)
        assertEquals(4, settings.guestDivisor)
        assertEquals(40, settings.renderScalePercent)
        assertEquals(4, settings.cpuLevel)
    }

    @Test fun guestEnvironmentFallsBackToLavapipe() {
        val env = OculusGo.guestEnv()
        assertTrue("BL_OCULUS_GO=1" in env)
        assertTrue("BL_VKBRIDGE_FALLBACK_ICD=${OculusGo.LAVAPIPE_ICD}" in env)
    }
}
