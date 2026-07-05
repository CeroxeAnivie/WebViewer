package top.ceroxe.webviewer.media;

import java.io.IOException;
import java.io.OutputStream;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicLong;

public final class DirectChunkOutputStream extends OutputStream {
    private final MediaChunkSink sink;
    private final AtomicLong sequence = new AtomicLong();

    public DirectChunkOutputStream(MediaChunkSink sink) {
        this.sink = Objects.requireNonNull(sink, "sink");
    }

    @Override
    public void write(int b) {
        write(new byte[]{(byte) b}, 0, 1);
    }

    @Override
    public synchronized void write(byte[] b, int off, int len) {
        if (b == null) {
            throw new NullPointerException("b");
        }
        if (off < 0 || len < 0 || len > b.length - off) {
            throw new IndexOutOfBoundsException("off=" + off + ", len=" + len + ", size=" + b.length);
        }
        if (len == 0) {
            return;
        }
        byte[] packet = new byte[len];
        System.arraycopy(b, off, packet, 0, len);
        int magic = packet.length >= Integer.BYTES ? EncodedVideoChunk.readU32(packet, 0) : 0;
        long nextSequence = sequence.incrementAndGet();
        if (magic == EncodedVideoChunk.MAGIC) {
            sink.publish(new EncodedVideoChunk(packet, nextSequence));
        } else if (magic == EncodedAudioChunk.MAGIC) {
            sink.publish(new EncodedAudioChunk(packet, nextSequence));
        } else {
            throw new IllegalArgumentException("Unknown native media packet");
        }
    }
}
