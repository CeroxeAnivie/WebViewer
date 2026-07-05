package top.ceroxe.webviewer.capture;

/**
 * Mirrors professional Windows capture selection: stay inside native graphics
 * capture methods and recover there, instead of falling back to unrelated
 * process-level recorders.
 */
public enum CaptureMethod {
    AUTO,
    WINDOWS_GRAPHICS_CAPTURE,
    DXGI_DESKTOP_DUPLICATION
}
