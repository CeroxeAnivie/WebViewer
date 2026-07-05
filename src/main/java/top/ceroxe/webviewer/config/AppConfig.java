package top.ceroxe.webviewer.config;

import java.net.InetSocketAddress;
import java.util.HashMap;
import java.util.Map;

public record AppConfig(
        String host,
        int httpPort,
        int websocketPort,
        int width,
        int height,
        int fps,
        int videoBitrate,
        int displayIndex,
        int adapterIndex,
        String password
) {
    private static final int MIN_AUTO_VIDEO_BITRATE = 4_000_000;
    private static final int MAX_AUTO_VIDEO_BITRATE = 80_000_000;
    private static final int BITRATE_STEP = 1_000_000;
    private static final int FULL_BITRATE_FPS = 60;
    private static final double HIGH_FPS_EXTRA_WEIGHT = 0.35;
    private static final double LIVE_BITS_PER_PIXEL_FRAME = 0.030;

    public AppConfig {
        requirePort(httpPort, "httpPort");
        requirePort(websocketPort, "websocketPort");
        requireRange(width, 320, 7680, "width");
        requireRange(height, 240, 4320, "height");
        requireRange(fps, 1, 240, "fps");
        requireRange(videoBitrate, 1_000_000, 200_000_000, "videoBitrate");
        requireRange(displayIndex, 0, 32, "displayIndex");
        requireRange(adapterIndex, -1, 32, "adapterIndex");
        if (host == null || host.isBlank()) {
            throw new IllegalArgumentException("host \u4e0d\u80fd\u4e3a\u7a7a");
        }
        if (password == null || password.isBlank()) {
            throw new IllegalArgumentException("password \u4e0d\u80fd\u4e3a\u7a7a");
        }
    }

    public static AppConfig fromArgs(String[] args) {
        Map<String, String> values = new HashMap<>();
        for (String arg : args) {
            if (!arg.startsWith("--") || !arg.contains("=")) {
                throw new IllegalArgumentException("\u53c2\u6570\u5fc5\u987b\u4f7f\u7528 --key=value \u683c\u5f0f: " + arg);
            }
            int separator = arg.indexOf('=');
            values.put(arg.substring(2, separator), arg.substring(separator + 1));
        }
        int httpPort = parseInt(values, "http-port", 8080);
        int width = parseInt(values, "width", 1920);
        int height = parseInt(values, "height", 1080);
        int fps = parseInt(values, "fps", 60);
        int displayIndex = parseInt(values, "display", 0);
        int adapterIndex = parseInt(values, "gpu", -1);
        int videoBitrate = values.containsKey("bitrate")
                ? parseInt(values, "bitrate", 0)
                : recommendedVideoBitrate(width, height, fps);

        return new AppConfig(
                values.getOrDefault("host", "0.0.0.0"),
                httpPort,
                parseInt(values, "ws-port", httpPort),
                width,
                height,
                fps,
                videoBitrate,
                displayIndex,
                adapterIndex,
                values.getOrDefault("password", "")
        );
    }

    public static int recommendedVideoBitrate(int width, int height, int fps) {
        double weightedFps = weightedFpsForLiveBitrate(Math.max(1, fps));
        double rawBitrate = (double) Math.max(1, width)
                * Math.max(1, height)
                * weightedFps
                * LIVE_BITS_PER_PIXEL_FRAME;
        long rounded = (long) Math.ceil(rawBitrate / BITRATE_STEP) * BITRATE_STEP;
        long clamped = Math.max(MIN_AUTO_VIDEO_BITRATE, Math.min(MAX_AUTO_VIDEO_BITRATE, rounded));
        return Math.toIntExact(clamped);
    }

    private static double weightedFpsForLiveBitrate(int fps) {
        if (fps <= FULL_BITRATE_FPS) {
            return fps;
        }
        return FULL_BITRATE_FPS + (fps - FULL_BITRATE_FPS) * HIGH_FPS_EXTRA_WEIGHT;
    }

    public InetSocketAddress httpAddress() {
        return new InetSocketAddress(host, httpPort);
    }

    public InetSocketAddress websocketAddress() {
        return new InetSocketAddress(host, websocketPort);
    }

    private static int parseInt(Map<String, String> values, String key, int defaultValue) {
        String value = values.get(key);
        if (value == null || value.isBlank()) {
            return defaultValue;
        }
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("--" + key + " \u5fc5\u987b\u662f\u6574\u6570\uff0c\u5b9e\u9645\u8f93\u5165: " + value, e);
        }
    }

    private static void requirePort(int port, String name) {
        requireRange(port, 1, 65535, name);
    }

    private static void requireRange(int value, int min, int max, String name) {
        if (value < min || value > max) {
            throw new IllegalArgumentException(name + " \u8d85\u51fa\u8303\u56f4 " + min + "-" + max + ": " + value);
        }
    }
}
