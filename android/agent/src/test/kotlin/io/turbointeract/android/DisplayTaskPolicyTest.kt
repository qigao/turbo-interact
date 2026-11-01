package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Test

class DisplayTaskPolicyTest {
    private val target = DisplayRef(1, 2, 7, 1, 1)
    private val pkg = "com.android.settings"

    @Test fun userVisibleMainTaskIsNeverMoved() {
        assertEquals(LaunchVerdict.MAIN_SCREEN_BUSY, DisplayTaskPolicy.decide(target, pkg,
            listOf(DisplayTaskInfo(0, pkg, true))))
    }

    @Test fun sameAppOnOtherVirtualDisplayIsRejected() {
        assertEquals(LaunchVerdict.OTHER_SCREEN_BUSY, DisplayTaskPolicy.decide(target, pkg,
            listOf(DisplayTaskInfo(9, pkg, false))))
    }

    @Test fun otherPackageOnMainDoesNotBlockLaunch() {
        assertEquals(LaunchVerdict.ALLOWED, DisplayTaskPolicy.decide(target, pkg,
            listOf(DisplayTaskInfo(0, "com.example.other", true))))
    }

    @Test fun nonVisibleOldMainTaskCanBeLaunched() {
        assertEquals(LaunchVerdict.ALLOWED, DisplayTaskPolicy.decide(target, pkg,
            listOf(DisplayTaskInfo(0, pkg, false))))
    }

    @Test fun invalidDisplayOrPackageIsRejected() {
        assertEquals(LaunchVerdict.INVALID_TARGET, DisplayTaskPolicy.decide(target.copy(displayId = 0), pkg, emptyList()))
        assertEquals(LaunchVerdict.INVALID_TARGET, DisplayTaskPolicy.decide(target, "", emptyList()))
        assertEquals(LaunchVerdict.INVALID_TARGET, DisplayTaskPolicy.decide(target, "com.android.settings;echo", emptyList()))
    }
}
