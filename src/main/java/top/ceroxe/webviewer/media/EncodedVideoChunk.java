package top.ceroxe.webviewer.media;

import java.util.Arrays;
import java.util.Objects;

public final class EncodedVideoChunk {
    public static final int MAGIC = 0x56535357;
    public static final int HEADER_BYTES = 40;
    public static final int CODEC_H264_ANNEX_B = 1;
    public static final int FLAG_KEYFRAME = 1;

    private final byte[] data;
    private final int width;
    private final int height;
    private final int codec;
    private final int flags;
    private final long timestampMicros;
    private final long sequence;

    public EncodedVideoChunk(byte[] data, long sequence) {
        this.data = Arrays.copyOf(Objects.requireNonNull(data, "data"), data.length);
        if (data.length < HEADER_BYTES || readU32(data, 0) != MAGIC) {
            throw new IllegalArgumentException("Invalid video packet");
        }
        int payloadBytes = readU32(data, 32);
        if (payloadBytes < 0 || data.length != HEADER_BYTES + payloadBytes) {
            throw new IllegalArgumentException("Video packet length mismatch");
        }
        this.codec = readU32(data, 8);
        this.flags = readU32(data, 12);
        this.width = readU32(data, 16);
        this.height = readU32(data, 20);
        this.timestampMicros = readU64(data, 24);
        this.sequence = sequence;
    }

    public byte[] data() {
        return Arrays.copyOf(data, data.length);
    }

    public byte[] payload() {
        int payloadBytes = readU32(data, 32);
        return Arrays.copyOfRange(data, HEADER_BYTES, HEADER_BYTES + payloadBytes);
    }

    public int width() {
        return width;
    }

    public int height() {
        return height;
    }

    public int codec() {
        return codec;
    }

    public boolean keyframe() {
        return (flags & FLAG_KEYFRAME) != 0;
    }

    public long timestampMicros() {
        return timestampMicros;
    }

    public long sequence() {
        return sequence;
    }

    static int readU32(byte[] data, int offset) {
        return (data[offset] & 0xff)
                | ((data[offset + 1] & 0xff) << 8)
                | ((data[offset + 2] & 0xff) << 16)
                | ((data[offset + 3] & 0xff) << 24);
    }

    static long readU64(byte[] data, int offset) {
        long value = 0;
        for (int i = 0; i < Long.BYTES; i++) {
            value |= (long) (data[offset + i] & 0xff) << (i * 8);
        }
        return value;
    }
}
