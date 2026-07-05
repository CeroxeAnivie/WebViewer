package top.ceroxe.webviewer.media;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class CmafMuxer {
    public static final String FORMAT = "wssv-cmaf-h264";
    public static final int FLAG_INIT = 1;
    public static final int FLAG_KEYFRAME = 2;
    private static final int TIMESCALE = 1_000_000;
    private static final int SEGMENT_MICROS = 500_000;

    private final List<Sample> pendingSamples = new ArrayList<>();
    private byte[] initSegment;
    private byte[] sps;
    private byte[] pps;
    private int width;
    private int height;
    private long baseTimestamp = Long.MIN_VALUE;
    private long lastTimestamp = Long.MIN_VALUE;
    private long lastDuration = 16_667;
    private long segmentStartTime;
    private int sequence = 1;

    public synchronized List<CmafSegment> accept(EncodedVideoChunk chunk) {
        if (chunk.codec() != EncodedVideoChunk.CODEC_H264_ANNEX_B) {
            return List.of();
        }
        List<CmafSegment> produced = new ArrayList<>();
        List<byte[]> nals = splitAnnexB(chunk.payload());
        if (chunk.keyframe() && initSegment == null) {
            sps = nals.stream().filter(CmafMuxer::isSps).findFirst().orElse(null);
            pps = nals.stream().filter(CmafMuxer::isPps).findFirst().orElse(null);
            if (sps == null || pps == null) {
                return List.of();
            }
            width = chunk.width();
            height = chunk.height();
            initSegment = initSegment(width, height, sps, pps);
            produced.add(CmafSegment.init(initSegment));
        }
        if (initSegment == null) {
            return List.of();
        }

        if (baseTimestamp == Long.MIN_VALUE) {
            baseTimestamp = chunk.timestampMicros();
            lastTimestamp = chunk.timestampMicros();
            segmentStartTime = 0;
        }

        long decodeTime = Math.max(0, chunk.timestampMicros() - baseTimestamp);
        long duration = Math.max(1, chunk.timestampMicros() - lastTimestamp);
        if (chunk.timestampMicros() == lastTimestamp) {
            duration = lastDuration;
        }
        lastTimestamp = chunk.timestampMicros();
        lastDuration = duration;

        List<byte[]> sampleNals = nals.stream()
                .filter(nal -> !isSps(nal) && !isPps(nal) && !isAud(nal))
                .toList();
        if (sampleNals.isEmpty()) {
            return produced;
        }
        if (!pendingSamples.isEmpty() && decodeTime - segmentStartTime >= SEGMENT_MICROS) {
            produced.add(flush(false));
            segmentStartTime = decodeTime;
            pendingSamples.add(new Sample(avccSample(sampleNals), duration, chunk.keyframe()));
            return produced;
        }

        pendingSamples.add(new Sample(avccSample(sampleNals), duration, chunk.keyframe()));
        return produced;
    }

    public synchronized byte[] initSegment() {
        return initSegment == null ? null : Arrays.copyOf(initSegment, initSegment.length);
    }

    private CmafSegment flush(boolean keyframeOnly) {
        byte[] segment = fragmentMp4(sequence++, segmentStartTime, pendingSamples);
        boolean keyframe = pendingSamples.stream().anyMatch(Sample::keyframe);
        pendingSamples.clear();
        return new CmafSegment(segment, keyframe || keyframeOnly, false);
    }

    private static boolean isSps(byte[] nal) {
        return nal.length > 0 && (nal[0] & 0x1f) == 7;
    }

    private static boolean isPps(byte[] nal) {
        return nal.length > 0 && (nal[0] & 0x1f) == 8;
    }

    private static boolean isAud(byte[] nal) {
        return nal.length > 0 && (nal[0] & 0x1f) == 9;
    }

    private static List<byte[]> splitAnnexB(byte[] data) {
        List<int[]> starts = new ArrayList<>();
        for (int i = 0; i + 3 < data.length; i++) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                starts.add(new int[]{i, 3});
                i += 2;
            } else if (i + 4 < data.length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                starts.add(new int[]{i, 4});
                i += 3;
            }
        }
        List<byte[]> nals = new ArrayList<>();
        for (int i = 0; i < starts.size(); i++) {
            int start = starts.get(i)[0] + starts.get(i)[1];
            int end = i + 1 < starts.size() ? starts.get(i + 1)[0] : data.length;
            if (end > start) {
                nals.add(Arrays.copyOfRange(data, start, end));
            }
        }
        return nals;
    }

    private static byte[] avccSample(List<byte[]> nals) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] nal : nals) {
            writeU32(out, nal.length);
            out.writeBytes(nal);
        }
        return out.toByteArray();
    }

    private static byte[] initSegment(int width, int height, byte[] sps, byte[] pps) {
        return concat(
                box("ftyp", ascii("isom"), u32(0x200), ascii("isom"), ascii("iso6"), ascii("avc1"), ascii("mp41")),
                box("moov",
                        box("mvhd", full(0, 0), u32(0), u32(0), u32(TIMESCALE), u32(0), u32(0x00010000), u16(0x0100), u16(0),
                                u32(0), u32(0), matrix(), u32(0), u32(0), u32(0), u32(0), u32(0), u32(0), u32(2)),
                        box("trak",
                                box("tkhd", full(0, 0x000007), u32(0), u32(0), u32(1), u32(0), u32(0), u32(0), u32(0),
                                        u16(0), u16(0), u16(0), u16(0), matrix(), u32(width << 16), u32(height << 16)),
                                box("mdia",
                                        box("mdhd", full(0, 0), u32(0), u32(0), u32(TIMESCALE), u32(0), u16(0x55c4), u16(0)),
                                        box("hdlr", full(0, 0), u32(0), ascii("vide"), u32(0), u32(0), u32(0), asciiz("VideoHandler")),
                                        box("minf",
                                                box("vmhd", full(0, 1), u16(0), u16(0), u16(0), u16(0)),
                                                box("dinf", box("dref", full(0, 0), u32(1), box("url ", full(0, 1)))),
                                                box("stbl",
                                                        box("stsd", full(0, 0), u32(1), avc1(width, height, sps, pps)),
                                                        box("stts", full(0, 0), u32(0)),
                                                        box("stsc", full(0, 0), u32(0)),
                                                        box("stsz", full(0, 0), u32(0), u32(0)),
                                                        box("stco", full(0, 0), u32(0)))))),
                        box("mvex", box("trex", full(0, 0), u32(1), u32(1), u32(0), u32(0), u32(0)))));
    }

    private static byte[] fragmentMp4(int sequence, long baseTime, List<Sample> samples) {
        ByteArrayOutputStream trunPayload = new ByteArrayOutputStream();
        writeU32(trunPayload, samples.size());
        writeU32(trunPayload, 0);
        ByteArrayOutputStream mdatPayload = new ByteArrayOutputStream();
        for (Sample sample : samples) {
            writeU32(trunPayload, Math.toIntExact(Math.min(0xffff_ffffL, sample.duration())));
            writeU32(trunPayload, sample.data().length);
            writeU32(trunPayload, sample.keyframe() ? 0x02000000 : 0x01010000);
            mdatPayload.writeBytes(sample.data());
        }
        byte[] moof = box("moof",
                box("mfhd", full(0, 0), u32(sequence)),
                box("traf",
                        box("tfhd", full(0, 0x020000), u32(1)),
                        box("tfdt", full(1, 0), u64(baseTime)),
                        box("trun", full(0, 0x000701), trunPayload.toByteArray())));
        int trunPayloadOffset = moof.length - trunPayload.size();
        int dataOffsetField = trunPayloadOffset + 4;
        writeU32(moof, dataOffsetField, moof.length + 8);
        return concat(moof, box("mdat", mdatPayload.toByteArray()));
    }

    private static byte[] avc1(int width, int height, byte[] sps, byte[] pps) {
        return box("avc1",
                new byte[6], u16(1), u16(0), u16(0), u32(0), u32(0), u32(0), u16(width), u16(height),
                u32(0x00480000), u32(0x00480000), u32(0), u16(1), new byte[32], u16(0x0018), u16(0xffff),
                box("avcC", new byte[]{1, sps[1], sps[2], sps[3], (byte) 0xff, (byte) 0xe1}, u16(sps.length), sps,
                        new byte[]{1}, u16(pps.length), pps));
    }

    private static byte[] box(String type, byte[]... payloads) {
        int size = 8;
        for (byte[] payload : payloads) {
            size += payload.length;
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream(size);
        writeU32(out, size);
        out.writeBytes(ascii(type));
        for (byte[] payload : payloads) {
            out.writeBytes(payload);
        }
        return out.toByteArray();
    }

    private static byte[] full(int version, int flags) {
        return new byte[]{(byte) version, (byte) (flags >>> 16), (byte) (flags >>> 8), (byte) flags};
    }

    private static byte[] matrix() {
        return concat(u32(0x00010000), u32(0), u32(0), u32(0), u32(0x00010000), u32(0), u32(0), u32(0), u32(0x40000000));
    }

    private static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] part : parts) {
            out.writeBytes(part);
        }
        return out.toByteArray();
    }

    private static byte[] ascii(String value) {
        return value.getBytes(StandardCharsets.ISO_8859_1);
    }

    private static byte[] asciiz(String value) {
        return concat(ascii(value), new byte[]{0});
    }

    private static byte[] u16(int value) {
        return new byte[]{(byte) (value >>> 8), (byte) value};
    }

    private static byte[] u32(int value) {
        byte[] out = new byte[4];
        writeU32(out, 0, value);
        return out;
    }

    private static byte[] u64(long value) {
        byte[] out = new byte[8];
        long v = value;
        for (int i = 7; i >= 0; i--) {
            out[i] = (byte) v;
            v >>>= 8;
        }
        return out;
    }

    private static void writeU32(ByteArrayOutputStream out, int value) {
        out.write((value >>> 24) & 0xff);
        out.write((value >>> 16) & 0xff);
        out.write((value >>> 8) & 0xff);
        out.write(value & 0xff);
    }

    private static void writeU32(byte[] out, int offset, int value) {
        out[offset] = (byte) (value >>> 24);
        out[offset + 1] = (byte) (value >>> 16);
        out[offset + 2] = (byte) (value >>> 8);
        out[offset + 3] = (byte) value;
    }

    private record Sample(byte[] data, long duration, boolean keyframe) {
    }

    public record CmafSegment(byte[] data, boolean keyframe, boolean init) {
        public static CmafSegment init(byte[] data) {
            return new CmafSegment(data, true, true);
        }
    }
}
