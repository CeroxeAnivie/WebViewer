# Native Capture Contract

WebViewer intentionally keeps Java out of the hot capture path.

The native module is built by Maven and bundled into the executable jar at
`/native/windows-x64/webviewer_capture.dll`.
At runtime it is extracted automatically to the user's local cache directory and loaded from there.
It can still be overridden for diagnostics with `-Dwebviewer.native=<absolute-path-to-dll>`.
The module owns the Windows capture pipeline:

- `AUTO` chooses between Windows Graphics Capture and DXGI Desktop Duplication.
- `WINDOWS_GRAPHICS_CAPTURE` is preferred for modern Windows and cross-GPU laptop cases.
- `DXGI_DESKTOP_DUPLICATION` is valid when the capture device and output are on the same graphics adapter.
- GDI and external process capture are not high-performance fallbacks.
- Device loss, display change, cursor composition, resize, black-frame detection, and encoder reset belong inside the native module.

JNI entrypoints expected by Java:

```c
JNIEXPORT void JNICALL Java_top_ceroxe_webviewer_capture_NativeScreenCaptureBackend_nativeStart(
    JNIEnv *env,
    jclass type,
    jstring method,
    jint width,
    jint height,
    jint fps,
    jint bitrate,
    jobject bridge);

JNIEXPORT void JNICALL Java_top_ceroxe_webviewer_capture_NativeScreenCaptureBackend_nativeStop(
    JNIEnv *env,
    jclass type);
```

The native module writes complete WebViewer video packets to `NativeBridge.write(byte[], int, int)`.
Payload is H.264 Annex-B byte stream produced by the Windows encoder stack; Java only authenticates,
queues, caches keyframes, and broadcasts packets.

Current packet layout is little-endian:

```text
0   uint32 magic "WSSV"
4   uint32 version = 1
8   uint32 codec = 1 (H.264 Annex-B)
12  uint32 flags bit 0 = keyframe
16  uint32 width
20  uint32 height
24  uint64 monotonic timestamp micros
32  uint32 payload bytes
36  uint32 reserved
40  H.264 payload
```
