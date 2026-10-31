package io.turbointeract.android;

import android.os.Bundle;
import android.view.Surface;

/**
 * Only an authenticated TurboInteract shell helper may implement this interface.
 * No arbitrary shell commands, main-screen actions, audio or clipboard APIs.
 */
interface ITurboDisplayShell {
    // Bundle contains explicit device/session/display/geometry generations.
    Bundle createDisplay(in Surface surface, int width, int height, int densityDpi);
    // 0=submitted, negative=typed rejection/unknown; never means task succeeded.
    int launchApp(int displayId, long displayGeneration, long geometryGeneration,
                  String packageName, long actionId);
    // 1=backend input sent, -1=denied, -2=unknown; never task verification.
    int tap(int displayId, long displayGeneration, long geometryGeneration,
            long actionId, long attemptId, int x, int y);
    // keyKind: 1=BACK, 2=ENTER; everything else rejected.
    int key(int displayId, long displayGeneration, long geometryGeneration,
            long actionId, long attemptId, int keyKind);
    // Teardown checks display incarnation. It is allowed even after grant revocation.
    boolean releaseDisplay(int displayId, long displayGeneration);
}
