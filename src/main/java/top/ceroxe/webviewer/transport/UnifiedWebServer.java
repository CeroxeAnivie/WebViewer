package top.ceroxe.webviewer.transport;

import top.ceroxe.webviewer.config.AppConfig;
import top.ceroxe.webviewer.media.CmafMuxer;
import top.ceroxe.webviewer.media.EncodedAudioChunk;
import top.ceroxe.webviewer.media.EncodedVideoChunk;
import top.ceroxe.webviewer.media.MediaChunkSink;
import top.ceroxe.webviewer.util.AppLog;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * A deliberately small single-port HTTP/WebSocket server.
 *
 * The important engineering point is that TCP port mapping tools only need one
 * public port: ordinary HTTP requests receive static assets, while
 * "Upgrade: websocket" on /stream is promoted to the binary H.264 stream.
 */
public final class UnifiedWebServer implements MediaChunkSink {
    private static final String RESOURCE_ROOT = "/static";
    private static final String WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    private static final int MAX_CLIENT_QUEUE = 24;
    private static final int MAX_CLIENT_CONTROL_PAYLOAD = 64 * 1024;
    private static final int MAX_SOCKET_SEND_BUFFER_BYTES = 512 * 1024;
    private static final int MIN_CLIENT_QUEUE_BYTES = 2 * 1024 * 1024;
    private static final int MAX_CLIENT_QUEUE_BYTES = 16 * 1024 * 1024;
    private static final double CLIENT_QUEUE_SECONDS = 2.0;
    private static final long STATS_INTERVAL_NANOS = 5_000_000_000L;
    private static final int MAX_FRAMES_PER_SOCKET_FLUSH = 12;
    private static final int HEARTBEAT_SECONDS = 15;

    private static final Map<String, String> MIME_TYPES = Map.of(
            "html", "text/html; charset=utf-8",
            "js", "application/javascript; charset=utf-8",
            "css", "text/css; charset=utf-8",
            "ico", "image/x-icon"
    );

    private final AppConfig config;
    private final Set<Client> clients = new CopyOnWriteArraySet<>();
    private final AtomicBoolean running = new AtomicBoolean();
    private final AtomicLong chunksPublished = new AtomicLong();
    private final AtomicLong droppedFrames = new AtomicLong();
    private final AtomicLong lastStatsNanos = new AtomicLong(System.nanoTime());
    private final AtomicLong lastStatsPublished = new AtomicLong();
    private final CmafMuxer muxer = new CmafMuxer();
    private final int maxClientQueuedBytes;

    private volatile byte[] latestInitSegment;
    private volatile byte[] latestKeySegment;
    private ServerSocket serverSocket;
    private Thread acceptThread;

    public UnifiedWebServer(AppConfig config) {
        this.config = Objects.requireNonNull(config, "config");
        this.maxClientQueuedBytes = maxClientQueuedBytes(config.videoBitrate());
    }

    public void start() throws IOException {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        serverSocket = new ServerSocket();
        serverSocket.setReuseAddress(true);
        serverSocket.bind(config.httpAddress());
        acceptThread = new Thread(this::acceptLoop, "webviewer-unified-accept");
        acceptThread.setDaemon(false);
        acceptThread.start();
        AppLog.info("Network", "HTTP/WebSocket listening on " + config.httpAddress());
    }

    public void stopGracefully() {
        running.set(false);
        closeQuietly(serverSocket);
        for (Client client : clients) {
            client.close();
        }
        clients.clear();
    }

    @Override
    public void publish(EncodedVideoChunk chunk) {
        for (CmafMuxer.CmafSegment segment : muxer.accept(chunk)) {
            publishSegment(segment);
        }
        logStreamStatsIfDue();
    }

    @Override
    public void publish(EncodedAudioChunk chunk) {
        byte[] data = AudioWirePacket.packet(chunk);
        for (Client client : clients) {
            long dropped = client.enqueueAudioPacket(data);
            if (dropped < 0) {
                clients.remove(client);
                client.close();
            } else if (dropped > 0) {
                droppedFrames.addAndGet(dropped);
            }
        }
    }

    private void publishSegment(CmafMuxer.CmafSegment segment) {
        byte[] data = segment.init()
                ? CmafWirePacket.init(segment.data())
                : CmafWirePacket.media(segment.data(), segment.keyframe());
        if (segment.init()) {
            latestInitSegment = Arrays.copyOf(segment.data(), segment.data().length);
        }
        if (!segment.init() && segment.keyframe()) {
            latestKeySegment = Arrays.copyOf(data, data.length);
        }
        chunksPublished.incrementAndGet();
        publishWirePacket(data, segment.keyframe(), !segment.init());
    }

    private void publishWirePacket(byte[] data, boolean keyframe, boolean droppable) {
        for (Client client : clients) {
            long dropped = client.enqueueVideoPacket(data, keyframe, droppable);
            if (dropped < 0) {
                clients.remove(client);
                client.close();
            } else if (dropped > 0) {
                droppedFrames.addAndGet(dropped);
            }
        }
    }

    private void acceptLoop() {
        while (running.get()) {
            try {
                Socket socket = serverSocket.accept();
                socket.setTcpNoDelay(true);
                socket.setKeepAlive(true);
                socket.setSendBufferSize(MAX_SOCKET_SEND_BUFFER_BYTES);
                Thread.startVirtualThread(() -> handle(socket));
            } catch (IOException e) {
                if (running.get()) {
                    AppLog.warn("Network", "accept failed: " + e.getMessage());
                }
            }
        }
    }

    private void handle(Socket socket) {
        try {
            BufferedInputStream input = new BufferedInputStream(socket.getInputStream());
            BufferedOutputStream output = new BufferedOutputStream(socket.getOutputStream());
            HttpRequest request = readRequest(input);
            if (request == null) {
                closeQuietly(socket);
                return;
            }
            if (isWebSocketUpgrade(request)) {
                handleWebSocket(socket, input, output, request);
            } else {
                serveHttp(socket, output, request);
            }
        } catch (Exception e) {
            closeQuietly(socket);
        }
    }

    private void handleWebSocket(Socket socket, BufferedInputStream input, BufferedOutputStream output,
                                 HttpRequest request) throws Exception {
        String path = request.pathWithoutQuery();
        if (!"/stream".equals(path)) {
            writeHttp(output, 404, "Not Found", "text/plain; charset=utf-8", "Not Found".getBytes(StandardCharsets.UTF_8));
            closeQuietly(socket);
            return;
        }
        String password = request.queryParam("pwd");
        if (!config.password().equals(password)) {
            writeHttp(output, 403, "Forbidden", "text/plain; charset=utf-8", "Auth failed".getBytes(StandardCharsets.UTF_8));
            closeQuietly(socket);
            return;
        }
        String key = request.header("sec-websocket-key");
        if (key == null || key.isBlank()) {
            writeHttp(output, 400, "Bad Request", "text/plain; charset=utf-8", "Missing WebSocket key".getBytes(StandardCharsets.UTF_8));
            closeQuietly(socket);
            return;
        }

        byte[] acceptBytes = MessageDigest.getInstance("SHA-1")
                .digest((key.trim() + WEBSOCKET_GUID).getBytes(StandardCharsets.ISO_8859_1));
        String accept = Base64.getEncoder().encodeToString(acceptBytes);
        String response = "HTTP/1.1 101 Switching Protocols\r\n"
                + "Upgrade: websocket\r\n"
                + "Connection: Upgrade\r\n"
                + "Sec-WebSocket-Accept: " + accept + "\r\n"
                + "\r\n";
        output.write(response.getBytes(StandardCharsets.ISO_8859_1));
        output.flush();

        Client client = new Client(socket, output, maxClientQueuedBytes);
        clients.add(client);
        client.start();
        client.enqueueText("{\"type\":\"hello\",\"format\":\"" + CmafMuxer.FORMAT + "\",\"version\":2,"
                + "\"width\":" + config.width() + ",\"height\":" + config.height()
                + ",\"fps\":" + config.fps() + ",\"bitrate\":" + config.videoBitrate() + "}");
        byte[] init = latestInitSegment;
        if (init != null) {
            client.enqueueVideoPacket(CmafWirePacket.init(init), true, false);
        }
        byte[] keySegment = latestKeySegment;
        if (keySegment != null) {
            client.enqueueVideoPacket(keySegment, true, true);
        }
        AppLog.info("Network", "client connected: " + socket.getRemoteSocketAddress()
                + ", viewers=" + clients.size());
        drainClientFrames(input, client);
    }

    private void drainClientFrames(BufferedInputStream input, Client client) {
        try {
            while (running.get() && client.open()) {
                int first = input.read();
                if (first < 0) {
                    break;
                }
                int second = input.read();
                if (second < 0) {
                    break;
                }
                int opcode = first & 0x0f;
                boolean masked = (second & 0x80) != 0;
                long length = second & 0x7f;
                if (length == 126) {
                    length = ((long) input.read() << 8) | input.read();
                } else if (length == 127) {
                    length = 0;
                    for (int i = 0; i < 8; i++) {
                        length = (length << 8) | input.read();
                    }
                }
                if (length < 0 || length > MAX_CLIENT_CONTROL_PAYLOAD) {
                    client.close();
                    break;
                }
                byte[] mask = masked ? input.readNBytes(4) : new byte[0];
                byte[] payload = input.readNBytes(Math.toIntExact(length));
                if (masked) {
                    for (int i = 0; i < payload.length; i++) {
                        payload[i] ^= mask[i & 3];
                    }
                }
                if (opcode == 0x8) {
                    client.closeGracefully(payload);
                    break;
                }
                if (opcode == 0x9) {
                    client.enqueueFrame(0xA, payload, false);
                }
            }
        } catch (Exception ignored) {
        } finally {
            clients.remove(client);
            client.close();
            AppLog.info("Network", "client disconnected, viewers=" + clients.size()
                    + ", clientDropped=" + client.droppedFrames());
        }
    }

    private void logStreamStatsIfDue() {
        long now = System.nanoTime();
        long previous = lastStatsNanos.get();
        if (now - previous < STATS_INTERVAL_NANOS || !lastStatsNanos.compareAndSet(previous, now)) {
            return;
        }

        long published = chunksPublished.get();
        long previousPublished = lastStatsPublished.getAndSet(published);
        long intervalNanos = Math.max(1, now - previous);
        long fps = Math.round((published - previousPublished) * 1_000_000_000.0 / intervalNanos);
        int viewers = clients.size();
        if (viewers == 0) {
            return;
        }
        int maxQueue = 0;
        long maxQueueBytes = 0;
        long clientDropped = 0;
        for (Client client : clients) {
            maxQueue = Math.max(maxQueue, client.queueSize());
            maxQueueBytes = Math.max(maxQueueBytes, client.queuedBytes());
            clientDropped += client.droppedFrames();
        }
        AppLog.info("Stream", "publishFps=" + fps
                + ", viewers=" + viewers
                + ", maxClientQueue=" + maxQueue + "/" + MAX_CLIENT_QUEUE
                + ", maxClientQueueBytes=" + (maxQueueBytes / 1024 / 1024) + "MiB/"
                + (maxClientQueuedBytes / 1024 / 1024) + "MiB"
                + ", droppedFrames=" + droppedFrames.get()
                + ", clientDroppedFrames=" + clientDropped);
    }

    private void serveHttp(Socket socket, BufferedOutputStream output, HttpRequest request) throws IOException {
        if (!"GET".equals(request.method()) && !"HEAD".equals(request.method())) {
            writeHttp(output, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                    "Method Not Allowed".getBytes(StandardCharsets.UTF_8));
            closeQuietly(socket);
            return;
        }
        String path = normalizePath(request.pathWithoutQuery());
        if ("/favicon.ico".equals(path)) {
            writeHttp(output, 204, "No Content", "text/plain; charset=utf-8", new byte[0]);
            closeQuietly(socket);
            return;
        }
        try (InputStream stream = UnifiedWebServer.class.getResourceAsStream(RESOURCE_ROOT + path)) {
            if (stream == null) {
                writeHttp(output, 404, "Not Found", "text/plain; charset=utf-8",
                        ("404 Not Found: " + path).getBytes(StandardCharsets.UTF_8));
                closeQuietly(socket);
                return;
            }
            byte[] body = stream.readAllBytes();
            writeHttp(output, 200, "OK", contentType(path), "HEAD".equals(request.method()) ? new byte[0] : body);
        }
        closeQuietly(socket);
    }

    private static HttpRequest readRequest(BufferedInputStream input) throws IOException {
        ByteArrayOutputStream header = new ByteArrayOutputStream(2048);
        int matched = 0;
        while (header.size() < 16 * 1024) {
            int value = input.read();
            if (value < 0) {
                return null;
            }
            header.write(value);
            matched = switch (matched) {
                case 0 -> value == '\r' ? 1 : 0;
                case 1 -> value == '\n' ? 2 : value == '\r' ? 1 : 0;
                case 2 -> value == '\r' ? 3 : 0;
                case 3 -> value == '\n' ? 4 : 0;
                default -> matched;
            };
            if (matched == 4) {
                break;
            }
        }
        String text = header.toString(StandardCharsets.ISO_8859_1);
        String[] lines = text.split("\r\n");
        if (lines.length == 0) {
            return null;
        }
        String[] first = lines[0].split(" ", 3);
        if (first.length < 2) {
            return null;
        }
        Map<String, String> headers = new LinkedHashMap<>();
        for (int i = 1; i < lines.length; i++) {
            int separator = lines[i].indexOf(':');
            if (separator > 0) {
                headers.put(lines[i].substring(0, separator).toLowerCase(Locale.ROOT),
                        lines[i].substring(separator + 1).trim());
            }
        }
        return new HttpRequest(first[0], first[1], headers);
    }

    private static boolean isWebSocketUpgrade(HttpRequest request) {
        String upgrade = request.header("upgrade");
        String connection = request.header("connection");
        return upgrade != null
                && "websocket".equalsIgnoreCase(upgrade)
                && connection != null
                && connection.toLowerCase(Locale.ROOT).contains("upgrade");
    }

    private static void writeHttp(BufferedOutputStream output, int status, String reason, String contentType, byte[] body)
            throws IOException {
        String response = "HTTP/1.1 " + status + " " + reason + "\r\n"
                + "Content-Type: " + contentType + "\r\n"
                + "Content-Length: " + body.length + "\r\n"
                + "Cache-Control: no-store\r\n"
                + "Connection: close\r\n"
                + "\r\n";
        output.write(response.getBytes(StandardCharsets.ISO_8859_1));
        output.write(body);
        output.flush();
    }

    private static String normalizePath(String rawPath) {
        String path = rawPath == null || rawPath.isBlank() || "/".equals(rawPath) ? "/index.html" : rawPath;
        if (path.contains("..") || path.contains("\\")) {
            return "/index.html";
        }
        return path;
    }

    private static String contentType(String path) {
        int dot = path.lastIndexOf('.');
        if (dot < 0 || dot == path.length() - 1) {
            return "application/octet-stream";
        }
        return MIME_TYPES.getOrDefault(path.substring(dot + 1), "application/octet-stream");
    }

    private static int maxClientQueuedBytes(int videoBitrate) {
        long bytes = Math.round(Math.max(1, videoBitrate) / 8.0 * CLIENT_QUEUE_SECONDS);
        return (int) Math.max(MIN_CLIENT_QUEUE_BYTES, Math.min(MAX_CLIENT_QUEUE_BYTES, bytes));
    }

    private static void closeQuietly(AutoCloseable closeable) {
        if (closeable != null) {
            try {
                closeable.close();
            } catch (Exception ignored) {
            }
        }
    }

    private record HttpRequest(String method, String target, Map<String, String> headers) {
        String header(String name) {
            return headers.get(name.toLowerCase(Locale.ROOT));
        }

        String pathWithoutQuery() {
            int query = target.indexOf('?');
            return query < 0 ? target : target.substring(0, query);
        }

        String queryParam(String key) {
            int query = target.indexOf('?');
            if (query < 0 || query == target.length() - 1) {
                return "";
            }
            String[] pairs = target.substring(query + 1).split("&");
            for (String pair : pairs) {
                int separator = pair.indexOf('=');
                if (separator > 0 && key.equals(pair.substring(0, separator))) {
                    return URLDecoder.decode(pair.substring(separator + 1), StandardCharsets.UTF_8);
                }
            }
            return "";
        }
    }

    private static final class Client {
        private final Socket socket;
        private final BufferedOutputStream output;
        private final int maxQueuedBytes;
        private final LinkedBlockingDeque<OutboundFrame> queue = new LinkedBlockingDeque<>(MAX_CLIENT_QUEUE);
        private final AtomicBoolean open = new AtomicBoolean(true);
        private final AtomicBoolean closing = new AtomicBoolean();
        private final AtomicLong droppedFrames = new AtomicLong();
        private final AtomicLong queuedBytes = new AtomicLong();
        private volatile boolean waitingForKeyframe;
        private Thread writerThread;

        private Client(Socket socket, BufferedOutputStream output, int maxQueuedBytes) {
            this.socket = socket;
            this.output = output;
            this.maxQueuedBytes = maxQueuedBytes;
        }

        boolean open() {
            return open.get() && !closing.get();
        }

        void start() {
            writerThread = Thread.startVirtualThread(this::writeLoop);
        }

        long enqueueText(String text) {
            return enqueueFrame(0x1, text.getBytes(StandardCharsets.UTF_8), false);
        }

        long enqueueVideoPacket(byte[] payload, boolean keyframe, boolean droppable) {
            return enqueueFrame(0x2, payload, keyframe, droppable, true);
        }

        long enqueueAudioPacket(byte[] payload) {
            return enqueueFrame(0x2, payload, false, true, false);
        }

        long enqueueFrame(int opcode, byte[] payload, boolean keyframe) {
            return enqueueFrame(opcode, payload, keyframe, false, false);
        }

        long enqueueFrame(int opcode, byte[] payload, boolean keyframe, boolean droppable, boolean videoPacket) {
            if (!open.get() || closing.get()) {
                return -1;
            }
            if (waitingForKeyframe && videoPacket && !keyframe) {
                droppedFrames.incrementAndGet();
                return 1;
            }
            long dropped = 0;
            if (videoPacket && keyframe) {
                dropped = removeDroppableFrames();
                waitingForKeyframe = false;
            }
            OutboundFrame frame = new OutboundFrame(opcode, payload, droppable);
            while ((queue.size() >= MAX_CLIENT_QUEUE || queuedBytes.get() + payload.length > maxQueuedBytes)
                    && removeOneQueuedFrame()) {
                dropped++;
            }

            if (dropped > 0 && videoPacket && !keyframe) {
                waitingForKeyframe = true;
                dropped++;
                droppedFrames.addAndGet(dropped);
                return dropped;
            }

            if (queue.size() >= MAX_CLIENT_QUEUE || queuedBytes.get() + payload.length > maxQueuedBytes) {
                if (droppable) {
                    waitingForKeyframe = videoPacket && !keyframe;
                    dropped++;
                    droppedFrames.addAndGet(dropped);
                    return dropped;
                }
                dropped += removeDroppableFrames();
            }
            if (!queue.offerLast(frame)) {
                if (droppable) {
                    waitingForKeyframe = videoPacket && !keyframe;
                    dropped++;
                    droppedFrames.addAndGet(dropped);
                    return dropped;
                }
                return -1;
            }
            queuedBytes.addAndGet(payload.length);
            if (dropped > 0) {
                droppedFrames.addAndGet(dropped);
            }
            return dropped;
        }

        private void subtractQueuedBytes(int length) {
            queuedBytes.updateAndGet(current -> current <= length ? 0 : current - length);
        }

        private long removeDroppableFrames() {
            long dropped = 0;
            for (OutboundFrame frame : queue) {
                if (frame.droppable() && queue.remove(frame)) {
                    subtractQueuedBytes(frame.payload().length);
                    dropped++;
                }
            }
            return dropped;
        }

        private boolean removeOneQueuedFrame() {
            for (OutboundFrame frame : queue) {
                if (frame.droppable() && queue.remove(frame)) {
                    subtractQueuedBytes(frame.payload().length);
                    return true;
                }
            }
            return false;
        }

        int queueSize() {
            return queue.size();
        }

        long queuedBytes() {
            return queuedBytes.get();
        }

        long droppedFrames() {
            return droppedFrames.get();
        }

        private void writeLoop() {
            try {
                while (open.get() || !queue.isEmpty()) {
                    OutboundFrame frame = open.get()
                            ? queue.pollFirst(HEARTBEAT_SECONDS, TimeUnit.SECONDS)
                            : queue.pollFirst();
                    if (frame == null) {
                        if (open.get()) {
                            writeFrame(0x9, new byte[0]);
                            output.flush();
                            continue;
                        }
                        break;
                    }
                    subtractQueuedBytes(frame.payload().length);
                    writeFrame(frame.opcode(), frame.payload());
                    if (frame.opcode() == 0x8) {
                        output.flush();
                        break;
                    }
                    int drained = 1;
                    while (drained < MAX_FRAMES_PER_SOCKET_FLUSH) {
                        OutboundFrame next = queue.pollFirst();
                        if (next == null) {
                            break;
                        }
                        subtractQueuedBytes(next.payload().length);
                        writeFrame(next.opcode(), next.payload());
                        if (next.opcode() == 0x8) {
                            output.flush();
                            return;
                        }
                        drained++;
                    }
                    output.flush();
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } catch (IOException e) {
                open.set(false);
            } finally {
                queue.clear();
                queuedBytes.set(0);
                closeQuietly(socket);
            }
        }

        private void writeFrame(int opcode, byte[] payload) throws IOException {
            output.write(0x80 | opcode);
            if (payload.length <= 125) {
                output.write(payload.length);
            } else if (payload.length <= 0xffff) {
                output.write(126);
                output.write((payload.length >>> 8) & 0xff);
                output.write(payload.length & 0xff);
            } else {
                output.write(127);
                long length = payload.length;
                for (int shift = 56; shift >= 0; shift -= 8) {
                    output.write((int) ((length >>> shift) & 0xff));
                }
            }
            output.write(payload);
        }

        void close() {
            if (closing.compareAndSet(false, true)) {
                open.set(false);
                queue.clear();
                queuedBytes.set(0);
                if (writerThread != null) {
                    writerThread.interrupt();
                }
                closeQuietly(socket);
            } else {
                open.set(false);
                closeQuietly(socket);
            }
        }

        void closeGracefully(byte[] closePayload) {
            if (!closing.compareAndSet(false, true)) {
                return;
            }
            if (!open.get()) {
                closeQuietly(socket);
                return;
            }
            queue.clear();
            queuedBytes.set(0);
            OutboundFrame closeFrame = new OutboundFrame(0x8, closePayload, false);
            queue.offerLast(closeFrame);
            queuedBytes.addAndGet(closePayload.length);
            open.set(false);
            if (writerThread != null) {
                try {
                    writerThread.join(300);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            closeQuietly(socket);
        }
    }

    private record OutboundFrame(int opcode, byte[] payload, boolean droppable) {
    }

    private static final class CmafWirePacket {
        private static final int MAGIC = 0x434d4146;
        private static final int HEADER_BYTES = 16;

        static byte[] init(byte[] payload) {
            return packet(payload, CmafMuxer.FLAG_INIT);
        }

        static byte[] media(byte[] payload, boolean keyframe) {
            return packet(payload, keyframe ? CmafMuxer.FLAG_KEYFRAME : 0);
        }

        private static byte[] packet(byte[] payload, int flags) {
            byte[] out = new byte[HEADER_BYTES + payload.length];
            putU32(out, 0, MAGIC);
            putU32(out, 4, 1);
            putU32(out, 8, flags);
            putU32(out, 12, payload.length);
            System.arraycopy(payload, 0, out, HEADER_BYTES, payload.length);
            return out;
        }

        private static void putU32(byte[] out, int offset, int value) {
            out[offset] = (byte) (value >>> 24);
            out[offset + 1] = (byte) (value >>> 16);
            out[offset + 2] = (byte) (value >>> 8);
            out[offset + 3] = (byte) value;
        }
    }

    private static final class AudioWirePacket {
        private static final int MAGIC = 0x50434d41;
        private static final int VERSION = 1;
        private static final int HEADER_BYTES = 24;

        static byte[] packet(EncodedAudioChunk chunk) {
            byte[] payload = chunk.payload();
            byte[] out = new byte[HEADER_BYTES + payload.length];
            putU32(out, 0, MAGIC);
            putU32(out, 4, VERSION);
            putU32(out, 8, chunk.sampleRate());
            putU32(out, 12, chunk.channels());
            putU32(out, 16, (int) chunk.timestampMicros());
            putU32(out, 20, payload.length);
            System.arraycopy(payload, 0, out, HEADER_BYTES, payload.length);
            return out;
        }

        private static void putU32(byte[] out, int offset, int value) {
            out[offset] = (byte) (value >>> 24);
            out[offset + 1] = (byte) (value >>> 16);
            out[offset + 2] = (byte) (value >>> 8);
            out[offset + 3] = (byte) value;
        }
    }
}
