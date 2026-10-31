package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DisplayRegistryTest {
    private val geometry = DisplayGeometry(720, 1600, 320)

    @Test fun neverAdmitMainDisplay() {
        val registry = DisplayRegistry(5, 9)
        try { registry.attach(0, geometry); error("display 0 admitted") }
        catch (expected: IllegalArgumentException) { assertTrue(expected.message!!.contains("forbidden")) }
    }

    @Test fun isolateSessionAndReusedDisplayNumber() {
        val registry = DisplayRegistry(5, 9)
        val old = registry.attach(7, geometry)
        assertNotNull(registry.current(old.ref))
        assertTrue(registry.detach(old.ref))
        val newer = registry.attach(7, geometry)
        assertNull(registry.current(old.ref))
        assertFalse(registry.detach(old.ref))
        assertTrue(newer.ref.displayGeneration > old.ref.displayGeneration)
        assertNull(DisplayRegistry(5, 10).current(newer.ref))
    }

    @Test fun invalidateOldCoordinatesOnGeometryChange() {
        val registry = DisplayRegistry(5, 9)
        val first = registry.attach(9, geometry)
        val second = registry.updateGeometry(first.ref, DisplayGeometry(1600, 720, 320, 1))!!
        assertEquals(first.ref.displayGeneration, second.ref.displayGeneration)
        assertTrue(second.ref.geometryGeneration > first.ref.geometryGeneration)
        assertNull(registry.current(first.ref))
        assertNotNull(registry.current(second.ref))
    }

    @Test fun duplicateDisplayAndDrainAreBounded() {
        val registry = DisplayRegistry(5, 9)
        val one = registry.attach(5, geometry)
        try { registry.attach(5, geometry); error("duplicate accepted") }
        catch (expected: IllegalStateException) { }
        val two = registry.attach(6, geometry)
        assertEquals(2, registry.drain().size)
        assertNull(registry.current(one.ref))
        assertNull(registry.current(two.ref))
    }
}
