package top.ceroxe.webviewer.capture;

public final class NativeCaptureException extends RuntimeException {
    public NativeCaptureException(String message) {
        super(message);
    }

    public NativeCaptureException(String message, Throwable cause) {
        super(message, cause);
    }
}
