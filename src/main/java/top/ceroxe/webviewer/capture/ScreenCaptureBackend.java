package top.ceroxe.webviewer.capture;

import top.ceroxe.webviewer.config.AppConfig;

import java.io.OutputStream;

public interface ScreenCaptureBackend {
    void start(AppConfig config, OutputStream output) throws Exception;

    void stop();
}
