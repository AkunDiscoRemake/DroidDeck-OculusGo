package com.droiddeck.launcher

/**
 * Immersive SteamVR on the Oculus Go through VrApi (libdroiddeckgo, app/src/main/cpp/govr). The
 * Go has no OpenXR runtime; the guest side is the same driver, tracking file and frame channel as
 * the Quest path.
 */
class OculusGoVrActivity : QuestXrActivity() {
    override fun onStop() {
        super.onStop()
        if (!isChangingConfigurations) finish()
    }
}
