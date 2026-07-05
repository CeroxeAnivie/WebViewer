package top.ceroxe.webviewer.media;

import java.util.Arrays;
import java.util.Objects;

public final class EncodedAudioChunk {
    public static final int MAGIC = 0x41535357;
    public static final int HEADER_BYTES = 40;
    public static final int CODEC_PCM_FLOAT32 = 1;

    private final byte[] data;
    private final int codec;
    private final int sampleRate;
    private final int channels;
    private final long timestampMicros;
    private final long sequence;

    public EncodedAudioChunk(byte[] data, long sequence) {
        this.data = Arrays.copyOf(Objects.requireNonNull(data, "data"), data.length);
        if (data.length < HEADER_BYTES || EncodedVideoChunk.readU32(data, 0) != MAGIC) {
            throw new IllegalArgumentException("Invalid audio packet");
        }
        int payloadBytes = EncodedVideoChunk.readU32(data, 32);
        if (payloadBytes < 0 || data.length != HEADER_BYTES + payloadBytes) {
            throw new IllegalArgumentException("Audio packet length mismatch");
        }
        this.codec = EncodedVideoChunk.readU32(data, 8);
        this.sampleRate = EncodedVideoChunk.readU32(data, 16);
        this.channels = EncodedVideoChunk.readU32(data, 20);
        this.timestampMicros = EncodedVideoChunk.readU64(data, 24);
        this.sequence = sequence;
    }

    public int codec() {
        return codec;
    }

    public int sampleRate() {
        return sampleRate;
    }

    public int channels() {
        return channels;
    }

    public long timestampMicros() {
        return timestampMicros;
    }

    public long sequence() {
        return sequence;
    }

    public byte[] payload() {
        int payloadBytes = EncodedVideoChunk.readU32(data, 32);
        return Arrays.copyOfRange(data, HEADER_BYTES, HEADER_BYTES + payloadBytes);
    }
}
