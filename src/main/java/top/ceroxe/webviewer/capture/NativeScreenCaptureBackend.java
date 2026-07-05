package top.ceroxe.webviewer.capture;

import top.ceroxe.webviewer.config.AppConfig;
import top.ceroxe.webviewer.util.AppLog;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.HexFormat;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Native-only capture backend. Java owns lifecycle and transport; native code
 * owns WGC/DXGI selection, D3D11 texture flow, cursor composition, device-loss
 * recovery, display-change recovery, and NVENC handoff.
 */
public final class NativeScreenCaptureBackend implements ScreenCaptureBackend {
    private static final String LIBRARY_PROPERTY = "webviewer.native";
    private static final String DEFAULT_LIBRARY_NAME = "webviewer_capture";
    private static final String BUNDLED_LIBRARY_RESOURCE = "/native/windows-x64/webviewer_capture.dll";

    private final AtomicBoolean running = new AtomicBoolean();
    private final CaptureMethod method;
    private NativeBridge bridge;

    public NativeScreenCaptureBackend() {
        this(CaptureMethod.AUTO);
    }

    public NativeScreenCaptureBackend(CaptureMethod method) {
        this.method = Objects.requireNonNull(method, "method");
    }

    @Override
    public void start(AppConfig config, OutputStream output) {
        Objects.requireNonNull(config, "config");
        Objects.requireNonNull(output, "output");
        if (!running.compareAndSet(false, true)) {
            return;
        }

        try {
            loadNativeLibrary();
            bridge = new NativeBridge(output);
            nativeStart(method.name(), config.width(), config.height(), config.fps(), config.videoBitrate(),
                    config.displayIndex(), config.adapterIndex(), bridge);
        } catch (RuntimeException | Error e) {
            running.set(false);
            throw e;
        }
    }

    @Override
    public void stop() {
        if (running.compareAndSet(true, false)) {
            nativeStop();
        }
    }

    private static void loadNativeLibrary() {
        String configured = System.getProperty(LIBRARY_PROPERTY);
        if (configured != null && !configured.isBlank()) {
            loadExistingLibrary(Path.of(configured));
            return;
        }

        Path extracted = extractBundledLibrary();
        if (extracted != null) {
            loadExistingLibrary(extracted);
            return;
        }

        try {
            System.loadLibrary(DEFAULT_LIBRARY_NAME);
        } catch (UnsatisfiedLinkError e) {
            String os = System.getProperty("os.name", "unknown").toLowerCase(Locale.ROOT);
            throw new NativeCaptureException(
                    "\u7f3a\u5c11 native capture library\u3002\u5f53\u524d\u7cfb\u7edf: " + os
                            + "\u3002\u8bf7\u63d0\u4f9b -D" + LIBRARY_PROPERTY + "=<absolute-path-to-dll>"
                            + "\uff0c\u6216\u786e\u8ba4 jar \u5185\u5305\u542b " + BUNDLED_LIBRARY_RESOURCE,
                    e);
        }
    }

    private static native void nativeStart(String method, int width, int height, int fps, int bitrate,
                                           int displayIndex, int adapterIndex, NativeBridge bridge);

    private static native void nativeStop();

    private static void loadExistingLibrary(Path library) {
        if (!Files.isRegularFile(library)) {
            throw new NativeCaptureException("Native capture library \u4e0d\u5b58\u5728: " + library);
        }
        System.load(library.toAbsolutePath().normalize().toString());
    }

    private static Path extractBundledLibrary() {
        try (InputStream stream = NativeScreenCaptureBackend.class.getResourceAsStream(BUNDLED_LIBRARY_RESOURCE)) {
            if (stream == null) {
                return null;
            }
            byte[] libraryBytes = stream.readAllBytes();
            String digest = sha256(libraryBytes).substring(0, 16);
            Path directory = nativeCacheDirectory();
            Files.createDirectories(directory);
            Path library = directory.resolve(DEFAULT_LIBRARY_NAME + "-" + digest + ".dll");
            if (!Files.isRegularFile(library)) {
                Path temporary = directory.resolve(DEFAULT_LIBRARY_NAME + "-" + digest + ".tmp");
                Files.write(temporary, libraryBytes);
                Files.move(temporary, library, StandardCopyOption.REPLACE_EXISTING);
            }
            return library;
        } catch (IOException e) {
            throw new NativeCaptureException("\u91ca\u653e\u5185\u7f6e native capture library \u5931\u8d25: " + e.getMessage(), e);
        }
    }

    private static String sha256(byte[] bytes) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException e) {
            throw new NativeCaptureException("\u5f53\u524d Java \u8fd0\u884c\u65f6\u4e0d\u652f\u6301 SHA-256", e);
        }
    }

    private static Path nativeCacheDirectory() {
        String localAppData = System.getenv("LOCALAPPDATA");
        if (localAppData != null && !localAppData.isBlank()) {
            return Path.of(localAppData, "WebViewer", "native");
        }
        return Path.of(System.getProperty("java.io.tmpdir"), "WebViewer", "native");
    }

    public static final class NativeBridge {
        private final OutputStream output;

        private NativeBridge(OutputStream output) {
            this.output = output;
        }

        public void write(byte[] data, int offset, int length) throws Exception {
            output.write(data, offset, length);
        }

        public void flush() throws Exception {
            output.flush();
        }

        public void status(String status) {
            if (isVerboseNativeStatus(status)) {
                AppLog.verbose("Capture", status);
            } else {
                AppLog.info("Capture", status);
            }
        }

        private static boolean isVerboseNativeStatus(String status) {
            return status.startsWith("frames submitted:")
                    || status.startsWith("frames encoded:")
                    || status.startsWith("capture new fps:")
                    || status.startsWith("encode fps:")
                    || status.startsWith("nvenc timing")
                    || status.startsWith("checking adapter ");
        }
    }
}
