# CameraOverride v0.4

Zygisk module redirecting camera ID `0` to ID `2` in Instagram, TikTok and Telegram.
The intended use is the ultrawide rear camera in Instagram's instant photo mode.
ID `2` is the existing default; the actual ultrawide ID and field of view must be
confirmed on the phone's firmware. A sensor's 0.6x label is not a camera ID.

## Install and check

1. Download **CameraOverride-zygisk** from the successful GitHub Actions build.
   The downloaded ZIP is directly installable in Magisk.
2. Install it over the previous CameraOverride module, enable Zygisk and reboot.
3. Restart Instagram and capture a clean log:

```sh
adb shell am force-stop com.instagram.android
adb logcat -c
adb logcat -s CamOverride
```

4. Open Instagram's instant photo camera and select the rear camera. The log
   should include both `method=getCameraCharacteristics` and
   `method=connectDevice ... camera 0 -> 2`. On a 64-bit Binder layout with
   stability, the latter reports `layout 8 offset=28`. The parser also accepts
   other Binder sizes by using the Binder API rather than hardcoded offsets.
5. Compare the view with the phone's normal camera at 0.6x, then take and send
   a test photo. A patched request proves redirection, not a successful camera
   session or that ID `2` is the ultrawide sensor.

This redirects the rear camera across Instagram's Java Camera2 modes, including
instant photo if it uses this path. It does not create a 0.6x button or isolate
redirection to a particular Instagram screen. Front camera ID `1` is unchanged.
Native C++ Binder paths and old Camera1 integer IDs are not hooked.

## Runtime control

As root, select a verified single-digit sensor ID, then force-stop Instagram:

```sh
adb shell su -c 'setprop debug.camoverride.id 2'
adb shell am force-stop com.instagram.android
```

Disable redirection without uninstalling, then force-stop Instagram:

```sh
adb shell su -c 'setprop debug.camoverride.id off'
adb shell am force-stop com.instagram.android
```

The property is temporary and resets on reboot. Disable/remove the module in
Magisk and reboot for a persistent rollback. If ID `2` gives the wrong sensor,
do not infer the ultrawide ID from its number: check the firmware's camera list
and capture `adb shell dumpsys media.camera` while the stock 0.6x camera is open.

## What changed

- Resolve transaction codes from the phone's `ICameraService.Stub` constants.
- Read and release the callback with `AParcel_readStrongBinder` before accessing
  the `connectDevice` camera ID, preserving the Binder object and its stability.
- Patch only known camera ID arguments, without scanning arbitrary parcel words.
- Restore the parcel position after every read/patch; retain the other arguments.
- Initialize empty diagnostic output and report read errors and argument offsets.
- Run host regression tests that reject primitive reads inside Binder objects.

Android build and host tests can run in CI. Physical camera output must be tested
on the phone; the build cannot establish sensor availability or Instagram output.

## v0.4 black screen diagnostics

The module logs the real Binder status reply for camera characteristics, opening,
stream creation and session configuration. It includes `ok`, `exception`,
`service_error`, `transport` and `message`, while restoring the reply position
so that Instagram receives the original reply unchanged. Camera frame submission
replies are logged only when they fail. No permission, identity or camera-service
checks are changed, and a rejected camera open is not masked as success.

After installing/rebooting, choose one target ID (start with `2`) and capture a
single attempt in Instagram. On Windows CMD:

```bat
adb shell su -c "setprop debug.camoverride.id 2"
adb shell am force-stop com.instagram.android
adb logcat -c
```

Open instant photo, wait for the error, then save the relevant logs:

```bat
adb logcat -d -v threadtime | findstr /i "CamOverride CameraService CameraProvider CameraDevice Camera3 CameraManager configureStreams" > camera-log.txt
adb shell dumpsys media.camera > camera-info.txt
```

These are diagnostics, not confirmation that the 0.6x sensor works. If
`connectDevice reply ... ok=0`, its message explains the open rejection. If it
returns `ok=1`, inspect stream/session errors and the camera service event log.
Do not keep changing target IDs within the same attempt: characteristic caches
and an existing session can then describe different devices.
