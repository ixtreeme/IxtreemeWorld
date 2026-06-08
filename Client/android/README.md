# IxtreemeWorld Android Build

## Prerequisites
1. Build the native library first:
   ```powershell
   cd D:\IxtreemeWorld\Client
   cmake --preset android-arm64-debug
   cmake --build build/android-arm64-debug
   ```
2. Confirm `Client/build/android-arm64-debug/libVulkanClear.so` exists.
3. Enable USB debugging on the phone and connect it by USB.
4. If Java or the Android SDK are not on your global PATH, set them for the shell:
   ```powershell
   $env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
   $env:ANDROID_HOME = "C:\Users\ixtre\AppData\Local\Android\Sdk"
   $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
   $env:PATH = "$env:JAVA_HOME\bin;$env:ANDROID_HOME\platform-tools;$env:PATH"
   ```

## APK Build
```powershell
cd D:\IxtreemeWorld\Client\android
.\gradlew.bat assembleDebug
```

APK output:
`Client/android/app/build/outputs/apk/debug/app-debug.apk`

## Install To Phone
```powershell
adb devices
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or through Gradle:
```powershell
.\gradlew.bat installDebug
```

## Logcat
```powershell
adb logcat -s IxtreemeClient:V RMLUI:V NativeActivity:V
```
