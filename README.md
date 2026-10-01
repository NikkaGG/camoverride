# CameraOverride v0.5

Zygisk module for rear-camera redirection in Instagram, TikTok and Telegram.
It redirects Java Camera2 service calls and the logical camera ID in capture
requests. It does not add a zoom button or restrict itself to one Instagram screen.

## Poco X4 Pro 5G / crDroid 9.6

The supplied camera dump identifies ID `2` as a 1600×1200 sensor (likely macro).
ID `3` has a 1.65 mm focal length and a 3.70048 mm sensor width, compared with
5.89 mm / 8.4 mm for the main sensor: the field of view corresponds to about
0.64x. ID `3` is therefore the ultrawide candidate and the new default.
Direct access and the actual output still need phone verification.
ID `4` is a logical camera listing physical IDs `0` and `3`.

The previous black-screen log was not an open failure: ID `2` opened, stream
creation and endConfigure succeeded, then submitRequestList returned
`Invalid camera request settings`. Android validates that the first logical ID
in each request matches the opened device. v0.5 fixes that mismatch.

## Install and test

Install the ZIP from the successful **CameraOverride-zygisk** Actions artifact
over the previous Magisk module, enable Zygisk, and reboot. On Windows CMD:

```bat
adb shell su -c "setprop debug.camoverride.id 3"
adb shell am force-stop com.instagram.android
adb logcat -c
adb logcat -s CamOverride
```

Open Instagram's instant photo, select the rear camera, compare its view to the
stock camera at 0.6x, and take a photo. Expected diagnostic lines include:

```text
connectDevice ... camera 0 -> 3
device session registered: logical camera 0 -> 3
submitRequestList requests=1 patched=1 camera=0->3 parse_status=0
```

If it fails, save a clean attempt without switching IDs within that session:

```bat
adb logcat -d -v threadtime | findstr /i "CamOverride CameraService CameraProvider CameraDevice Camera3 CameraManager configureStreams" > camera-log.txt
```

Return the log. A successful patch alone does not prove the sensor opens or
produces frames. A rejected open retains the real service error.

## Runtime control

Select a single-digit ID, then force-stop Instagram:

```bat
adb shell su -c "setprop debug.camoverride.id 3"
adb shell am force-stop com.instagram.android
```

To disable, use `setprop debug.camoverride.id off`, then force-stop the app.
The property resets on reboot; remove/disable the module in Magisk and reboot
for persistent rollback. An already-open session retains its original target
until disconnected, even if the property changes.

## Implementation and validation

- Resolve service/device transaction constants from the phone's generated Stub.
- Consume callback Binder objects through the NDK Binder API, including stability.
- Track a successful redirected open by the returned device Binder's weak Java
  identity; leave front-camera and unrelated device requests unchanged.
- Parse single requests and arrays, changing only the first logical ID in each.
  Preserve physical IDs, metadata blobs, surfaces, cached stream indices and tags.
- Use the firmware's CameraMetadataNative/Surface decoders for opaque payloads.
  Close temporary native objects and restore every parcel/reply position.
- Validate the full request array before writing. Unknown/truncated formats are
  left unchanged and reported with parse_status.
- Log actual service errors without changing permissions or masking failures.
- Host regression checks cover protected Binder objects, burst requests, metadata
  preservation, front IDs, old/new tags, disabled mode, and every array truncation.

Native C++ Binder paths and Camera1 integer IDs are not hooked. Host tests and
the ARM64 Android CI build cannot verify the physical camera image.
