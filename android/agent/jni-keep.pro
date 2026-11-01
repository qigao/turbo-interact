# Native JNI symbols and peer callback method IDs are resolved by exact name.
# This is PRODUCT AAR minifier policy, not a consumer project or CI test.
-keep class io.turbointeract.android.NativeActionJni { *; }
-keep class io.turbointeract.android.NativeActionPeer { *; }
