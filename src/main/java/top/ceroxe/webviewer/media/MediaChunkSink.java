package top.ceroxe.webviewer.media;

public interface MediaChunkSink {
    void publish(EncodedVideoChunk chunk);

    default void publish(EncodedAudioChunk chunk) {
    }
}
