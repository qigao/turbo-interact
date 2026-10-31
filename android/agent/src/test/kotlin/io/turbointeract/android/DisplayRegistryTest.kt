package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DisplayRegistryTest {
    private val geometry = DisplayGeometry(720, 1600, 320)

    @Test fun rejectMainAndInvalidDisplay() {
        val registry = DisplayRegistry(5, 9)
        for (id in listOf(-1, 0)) {
            try { registry.attach(id, geometry); error("display $id admitted") }
            catch (expected: IllegalArgumentException) { assertTrue(expected.message!!.contains("forbidden")) }
        }
    }

    @Test fun reusedDisplayNumberInvalidatesOldRef() {
        val registry = DisplayRegistry(5, 9)
        val old = registry.attach(7, geometry)
        assertNotNull(registry.current(old.ref))
        assertTrue(registry.detach(old.ref))
        val newer = registry.attach(7, geometry)
        assertNull(registry.current(old.ref))
        assertFalse(registry.detachInstance(old.ref))
        assertTrue(newer.ref.displayGeneration > old.ref.displayGeneration)
        assertNull(DisplayRegistry(5, 10).current(newer.ref))
    }

    @Test fun newGeometryInvalidatesCoordinatesButNotDisplayLifetime() {
        val registry = DisplayRegistry(5, 9)
        val first = registry.attach(9, geometry)
        val second = registry.updateGeometry(first.ref, DisplayGeometry(1600, 720, 320, 1))!!
        assertEquals(first.ref.displayGeneration, second.ref.displayGeneration)
        assertTrue(second.ref.geometryGeneration > first.ref.geometryGeneration)
        assertNull(registry.current(first.ref))
        assertTrue(registry.ownsInstance(first.ref))
        assertFalse(registry.detach(first.ref))
        assertTrue(registry.detachInstance(first.ref))
        assertNull(registry.current(second.ref))
    }

    @Test fun doubleResizeCannotUseStaleGeometryAndDrainClears() {
        val registry = DisplayRegistry(5, 9)
        val first = registry.attach(5, geometry)
        val second = registry.updateGeometry(first.ref, DisplayGeometry(720, 1200, 320))!!
        assertNull(registry.updateGeometry(first.ref, geometry))
        val third = registry.updateGeometry(second.ref, DisplayGeometry(720, 1100, 320))!!
        assertEquals(3L, third.ref.geometryGeneration)
        val other = registry.attach(6, geometry)
        assertEquals(2, registry.drain().size)
        assertNull(registry.current(third.ref))
        assertNull(registry.current(other.ref))
    }

    @Test fun duplicateDisplayRejected() {
        val registry = DisplayRegistry(5, 9)
        registry.attach(5, geometry)
        try { registry.attach(5, geometry); error("duplicate accepted") }
        catch (expected: IllegalStateException) { }
    }
}
