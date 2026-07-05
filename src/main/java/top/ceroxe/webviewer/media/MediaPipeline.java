package top.ceroxe.webviewer.media;

import top.ceroxe.webviewer.capture.ScreenCaptureBackend;
import top.ceroxe.webviewer.config.AppConfig;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MediaPipeline {
    private final AppConfig config;
    private final ScreenCaptureBackend captureBackend;
    private final MediaChunkSink sink;
    private final AtomicBoolean running = new AtomicBoolean();

    public MediaPipeline(AppConfig config, ScreenCaptureBackend captureBackend, MediaChunkSink sink) {
        this.config = Objects.requireNonNull(config, "config");
        this.captureBackend = Objects.requireNonNull(captureBackend, "captureBackend");
        this.sink = Objects.requireNonNull(sink, "sink");
    }

    public void start() throws Exception {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        try {
            captureBackend.start(config, new DirectChunkOutputStream(sink));
        } catch (Exception e) {
            running.set(false);
            throw e;
        }
    }

    public void stop() {
        if (running.compareAndSet(true, false)) {
            captureBackend.stop();
        }
    }
}
