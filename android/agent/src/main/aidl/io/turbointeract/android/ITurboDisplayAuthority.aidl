package io.turbointeract.android;

/**
 * Live host Action Owner admission. A shell helper cannot authorize itself.
 * Binder is never exposed through a public network listener.
 */
interface ITurboDisplayAuthority {
    boolean allowCreate(long deviceSessionId, long sessionGeneration);
    boolean allowLaunch(long deviceSessionId, long sessionGeneration,
                        int displayId, long displayGeneration, long geometryGeneration,
                        String packageName, long actionId);
    // actionKind: 1 TAP, 2 BACK, 3 ENTER. All other kinds rejected.
    boolean allowInput(long deviceSessionId, long sessionGeneration,
                       int displayId, long displayGeneration, long geometryGeneration,
                       long actionId, long attemptId, int actionKind);
}
