package top.ceroxe.webviewer;

import top.ceroxe.webviewer.capture.NativeCaptureException;
import top.ceroxe.webviewer.capture.NativeScreenCaptureBackend;
import top.ceroxe.webviewer.config.AppConfig;
import top.ceroxe.webviewer.media.MediaPipeline;
import top.ceroxe.webviewer.transport.UnifiedWebServer;

import java.awt.GraphicsDevice;
import java.awt.GraphicsEnvironment;
import java.awt.Rectangle;
import java.io.PrintStream;
import java.net.InetAddress;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Scanner;

/**
 * WebViewer starts a small Java control plane and keeps the heavy screen
 * work behind the native D3D11/NVENC backend.
 */
public final class App {
    private static final String CONSOLE_CHARSET_PROPERTY = "webviewer.consoleCharset";

    private static Charset consoleCharset = StandardCharsets.UTF_8;

    private App() {
    }

    public static void main(String[] args) {
        configureConsoleEncoding();
        try {
            AppConfig config = readConfig(args);
            UnifiedWebServer webServer = new UnifiedWebServer(config);
            MediaPipeline pipeline = new MediaPipeline(config, new NativeScreenCaptureBackend(), webServer);

            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                pipeline.stop();
                webServer.stopGracefully();
            }, "webviewer-shutdown"));

            pipeline.start();
            webServer.start();

            printStartup(config);
            Thread.currentThread().join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (NativeCaptureException e) {
            System.err.println("[Fatal] WebViewer \u542f\u52a8\u5931\u8d25: " + e.getMessage());
            System.exit(1);
        } catch (Exception e) {
            System.err.println("[Fatal] WebViewer \u542f\u52a8\u5931\u8d25: " + e.getMessage());
            e.printStackTrace(System.err);
            System.exit(1);
        }
    }

    private static void configureConsoleEncoding() {
        String configuredCharset = System.getProperty(CONSOLE_CHARSET_PROPERTY);
        if (configuredCharset != null && !configuredCharset.isBlank()) {
            consoleCharset = Charset.forName(configuredCharset);
        } else if (System.console() != null) {
            consoleCharset = System.console().charset();
        }
        System.setOut(new PrintStream(System.out, true, consoleCharset));
        System.setErr(new PrintStream(System.err, true, consoleCharset));
    }

    private static AppConfig readConfig(String[] args) {
        if (args.length > 0) {
            return AppConfig.fromArgs(args);
        }

        Scanner scanner = new Scanner(System.in, consoleCharset);
        System.out.println("=========================================");
        System.out.println("   WebViewer 2.0");
        System.out.println("   Core: Lightweight TCP / Native Capture");
        System.out.println("=========================================");

        List<DisplayChoice> displays = displays();
        printDisplays(displays);
        int displayIndex = readInt(scanner, "\u9009\u62e9\u663e\u793a\u5668 (\u9ed8\u8ba4 0): ", 0, 0, displays.size() - 1);
        DisplayChoice display = displays.get(displayIndex);

        List<String> adapters = adapterNames();
        printAdapters(adapters);
        int adapterIndex = readInt(scanner, "\u9009\u62e9\u91c7\u96c6 GPU (-1 \u81ea\u52a8\uff0c\u9ed8\u8ba4 -1): ", -1, -1,
                Math.max(-1, adapters.size() - 1));

        ResolutionChoice resolution = readResolution(scanner, display);
        int fps = readInt(scanner, "\u8f93\u5165\u5e27\u7387 (\u9ed8\u8ba4 60, \u8303\u56f4 1-240): ", 60, 1, 240);
        int recommendedBitrateMbps = AppConfig.recommendedVideoBitrate(resolution.width(), resolution.height(), fps) / 1_000_000;
        int bitrateMbps = readInt(scanner, "\u8f93\u5165\u7801\u7387 Mbps (\u63a8\u8350 " + recommendedBitrateMbps
                + ", \u8303\u56f4 1-200): ", recommendedBitrateMbps, 1, 200);

        System.out.print("\u8bbe\u7f6e\u8bbf\u95ee\u5bc6\u7801: ");
        String password = cleanConsoleInput(scanner.nextLine());
        if (password.isEmpty()) {
            throw new IllegalArgumentException("\u5bc6\u7801\u4e0d\u80fd\u4e3a\u7a7a\u3002");
        }

        return new AppConfig("0.0.0.0", 8080, 8081, resolution.width(), resolution.height(), fps,
                bitrateMbps * 1_000_000, displayIndex, adapterIndex, password);
    }

    private static List<DisplayChoice> displays() {
        GraphicsDevice[] devices = GraphicsEnvironment.getLocalGraphicsEnvironment().getScreenDevices();
        List<DisplayChoice> displays = new ArrayList<>();
        for (int i = 0; i < devices.length; i++) {
            Rectangle bounds = devices[i].getDefaultConfiguration().getBounds();
            int width = devices[i].getDisplayMode() == null ? bounds.width : devices[i].getDisplayMode().getWidth();
            int height = devices[i].getDisplayMode() == null ? bounds.height : devices[i].getDisplayMode().getHeight();
            displays.add(new DisplayChoice(i, devices[i].getIDstring(), width, height));
        }
        if (displays.isEmpty()) {
            displays.add(new DisplayChoice(0, "Primary", 1920, 1080));
        }
        return displays;
    }

    private static void printDisplays(List<DisplayChoice> displays) {
        System.out.println();
        System.out.println("\u53ef\u7528\u663e\u793a\u5668:");
        for (DisplayChoice display : displays) {
            System.out.println(" [" + display.index() + "] " + display.name()
                    + "  " + display.width() + "x" + display.height());
        }
    }

    private static List<String> adapterNames() {
        List<String> adapters = new ArrayList<>();
        try {
            Process process = new ProcessBuilder("cmd.exe", "/d", "/c",
                    "chcp 65001 >nul & wmic path win32_VideoController get Name /value")
                    .redirectErrorStream(true)
                    .start();
            String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            process.waitFor();
            for (String line : output.split("\\R")) {
                String trimmed = line.trim();
                if (trimmed.startsWith("Name=") && trimmed.length() > "Name=".length()) {
                    adapters.add(trimmed.substring("Name=".length()));
                }
            }
        } catch (Exception ignored) {
        }
        if (adapters.isEmpty()) {
            adapters.add("GPU 0");
        }
        return adapters;
    }

    private static void printAdapters(List<String> adapters) {
        System.out.println();
        System.out.println("\u53ef\u7528\u91c7\u96c6 GPU:");
        System.out.println(" [-1] \u81ea\u52a8 (\u4f18\u5148\u72ec\u7acb\u663e\u5361)");
        for (int i = 0; i < adapters.size(); i++) {
            System.out.println(" [" + i + "] " + adapters.get(i));
        }
    }

    private static ResolutionChoice readResolution(Scanner scanner, DisplayChoice display) {
        List<ResolutionChoice> choices = new ArrayList<>();
        choices.add(fitResolution("1K", display.width(), display.height(), 1920, 1080));
        if (display.width() >= 2560 || display.height() >= 1440) {
            choices.add(fitResolution("2K", display.width(), display.height(), 2560, 1440));
        }
        choices.add(new ResolutionChoice("\u539f\u753b", even(display.width()), even(display.height())));

        System.out.println();
        System.out.println("\u9009\u62e9\u5206\u8fa8\u7387:");
        for (int i = 0; i < choices.size(); i++) {
            ResolutionChoice choice = choices.get(i);
            System.out.println(" [" + i + "] " + choice.label() + "  " + choice.width() + "x" + choice.height());
        }
        int selected = readInt(scanner, "\u9009\u62e9\u5206\u8fa8\u7387 (\u9ed8\u8ba4 " + (choices.size() - 1) + "): ",
                choices.size() - 1, 0, choices.size() - 1);
        return choices.get(selected);
    }

    private static ResolutionChoice fitResolution(String label, int sourceWidth, int sourceHeight, int maxWidth, int maxHeight) {
        double scale = Math.min((double) maxWidth / sourceWidth, (double) maxHeight / sourceHeight);
        scale = Math.min(1.0, scale);
        return new ResolutionChoice(label, even((int) Math.round(sourceWidth * scale)),
                even((int) Math.round(sourceHeight * scale)));
    }

    private static int even(int value) {
        return Math.max(2, value & ~1);
    }

    private static int readInt(Scanner scanner, String prompt, int defaultValue, int min, int max) {
        System.out.print(prompt);
        String value = cleanConsoleInput(scanner.nextLine());
        if (value.isEmpty()) {
            return defaultValue;
        }
        try {
            int parsed = Integer.parseInt(value);
            if (parsed < min || parsed > max) {
                throw new IllegalArgumentException(prompt + "\u5141\u8bb8\u8303\u56f4\u662f " + min + "-" + max
                        + "\uff0c\u5b9e\u9645\u8f93\u5165: " + parsed);
            }
            return parsed;
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(prompt + "\u5fc5\u987b\u662f\u6574\u6570\uff0c\u5b9e\u9645\u8f93\u5165: " + value, e);
        }
    }

    private static String cleanConsoleInput(String value) {
        if (value == null) {
            return "";
        }
        return value.replace("\ufeff", "").trim();
    }

    private static void printStartup(AppConfig config) throws Exception {
        String localIp = InetAddress.getLocalHost().getHostAddress();
        System.out.println();
        System.out.println("=========================================");
        System.out.println(" WebViewer \u5df2\u542f\u52a8");
        System.out.println(" \u672c\u673a\u9875\u9762: http://localhost:" + config.httpPort());
        System.out.println(" \u5c40\u57df\u7f51\u9875\u9762: http://" + localIp + ":" + config.httpPort());
        System.out.println(" \u5a92\u4f53\u5730\u5740: ws://" + localIp + ":" + config.httpPort() + "/stream");
        System.out.println(" \u5206\u8fa8\u7387:   " + config.width() + "x" + config.height());
        System.out.println(" \u663e\u793a\u5668:   " + config.displayIndex());
        System.out.println(" \u91c7\u96c6 GPU: " + (config.adapterIndex() < 0 ? "\u81ea\u52a8" : config.adapterIndex()));
        System.out.println(" \u5e27\u7387:     " + config.fps() + " FPS");
        System.out.println(" \u7801\u7387:     " + (config.videoBitrate() / 1_000_000) + " Mbps");
        System.out.println("=========================================");
    }

    private record DisplayChoice(int index, String name, int width, int height) {
    }

    private record ResolutionChoice(String label, int width, int height) {
    }
}
