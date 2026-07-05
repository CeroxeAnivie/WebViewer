package top.ceroxe.webviewer.util;

/**
 * Tiny logging facade for the single-process desktop/server hybrid.
 *
 * Keeping logging here avoids pulling a full logging stack into the hot path.
 * High-frequency capture/network diagnostics are opt-in because console I/O is
 * slow enough to perturb a 120-150 FPS screen pipeline.
 */
public final class AppLog {
    private static final boolean VERBOSE = Boolean.getBoolean("webviewer.verbose");

    private AppLog() {
    }

    public static boolean verboseEnabled() {
        return VERBOSE;
    }

    public static void info(String component, String message) {
        System.out.println("[" + component + "] " + message);
    }

    public static void verbose(String component, String message) {
        if (VERBOSE) {
            info(component, message);
        }
    }

    public static void warn(String component, String message) {
        System.err.println("[" + component + "] WARN " + message);
    }

    public static void error(String component, String message) {
        System.err.println("[" + component + "] ERROR " + message);
    }
}
