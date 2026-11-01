# Native JNI symbols and peer callback method IDs are resolved by exact name.
# This is PRODUCT AAR minifier policy, not a consumer project or CI test.
-keep class io.turbointeract.android.NativeActionJni { *; }
-keep class io.turbointeract.android.NativeActionPeer { *; }

# app_process loads this class by the literal name in ShellLaunchCommand.
# It is not referenced by a normal Java call from the host APK and can be
# removed or renamed by R8 unless the product AAR provides a consumer rule.
# Preserve the exact public static main(String[]) signature, even when an
# embedding host enables minifyEnabled. This is NOT a consumer test project.
-keep class io.turbointeract.android.TurboDisplayShellMain {
    public static void main(java.lang.String[]);
}
