'use strict';

const CMAF_MAGIC = 0x434d4146;
const PCM_MAGIC = 0x50434d41;
const HEADER_BYTES = 16;
const AUDIO_HEADER_BYTES = 24;
const FLAG_INIT = 1;
const FLAG_KEYFRAME = 2;
const MAX_APPEND_QUEUE_BYTES = 10 * 1024 * 1024;
const MAX_APPEND_QUEUE_SEGMENTS = 8;
const START_BUFFER_SECONDS = 2.0;
const TARGET_LATENCY_SECONDS = 2.5;
const MAX_LATENCY_SECONDS = 5.0;
const PANIC_LATENCY_SECONDS = 7.0;
const BACK_BUFFER_SECONDS = 8.0;
const RECONNECT_DELAY_MS = 800;

const video = document.getElementById('screen');
const viewport = document.querySelector('.viewport');
const statusLine = document.getElementById('status');
const passwordInput = document.getElementById('password');
const connectButton = document.getElementById('connect');
const fullscreenButton = document.getElementById('fullscreen');

let socket = null;
let mediaSource = null;
let sourceBuffer = null;
let objectUrl = null;
let appendQueue = [];
let appendQueueBytes = 0;
let initialized = false;
let width = 0;
let height = 0;
let frames = 0;
let waitingForKeyframe = true;
let waitingForStartupBuffer = true;
let lastFpsTime = performance.now();
let audioContext = null;
let audioNextTime = 0;
let resizeObserver = null;
let reconnectTimer = 0;
let userDisconnecting = false;

function setStatus(text, error = false) {
    statusLine.textContent = text;
    statusLine.style.color = error ? 'var(--danger)' : 'var(--muted)';
}

function wsUrl(password) {
    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${protocol}//${location.host}/stream?pwd=${encodeURIComponent(password)}`;
}

function connect() {
    const password = passwordInput.value.trim();
    if (!password) {
        setStatus('\u5bc6\u7801\u4e0d\u80fd\u4e3a\u7a7a', true);
        return;
    }

    disconnect();
    resetPlayback();
    ensureAudioContext();
    userDisconnecting = false;
    connectButton.disabled = true;
    setStatus('\u6b63\u5728\u8fde\u63a5');

    socket = new WebSocket(wsUrl(password));
    socket.binaryType = 'arraybuffer';
    socket.onopen = () => setStatus('\u5df2\u8fde\u63a5\uff0c\u7b49\u5f85\u5a92\u4f53\u521d\u59cb\u5316');
    socket.onmessage = event => {
        if (typeof event.data !== 'string') {
            receivePacket(event.data);
        }
    };
    socket.onerror = () => setStatus('\u8fde\u63a5\u5f02\u5e38', true);
    socket.onclose = event => {
        connectButton.disabled = false;
        setStatus(`\u8fde\u63a5\u5173\u95ed ${event.code} ${event.reason || ''}`, event.code !== 1000);
    };
}

function disconnect() {
    userDisconnecting = true;
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = 0;
    }
    if (socket) {
        socket.close(1000, 'Reconnect');
        socket = null;
    }
}

function reconnectAfterPlaybackFailure(reason) {
    if (userDisconnecting || reconnectTimer) {
        return;
    }
    setStatus(`\u64ad\u653e\u7ba1\u7ebf\u6062\u590d\u4e2d: ${reason}`, true);
    reconnectTimer = window.setTimeout(() => {
        reconnectTimer = 0;
        if (socket) {
            socket.close(1011, 'Media pipeline reset');
            socket = null;
        }
        connect();
    }, RECONNECT_DELAY_MS);
}

function resetPlayback() {
    appendQueue = [];
    appendQueueBytes = 0;
    sourceBuffer = null;
    initialized = false;
    waitingForKeyframe = true;
    waitingForStartupBuffer = true;
    frames = 0;
    if (mediaSource && mediaSource.readyState === 'open') {
        try {
            mediaSource.endOfStream();
        } catch {
            // endOfStream is best-effort during reconnect.
        }
    }
    if (objectUrl) {
        URL.revokeObjectURL(objectUrl);
        objectUrl = null;
    }
    mediaSource = new MediaSource();
    objectUrl = URL.createObjectURL(mediaSource);
    video.removeAttribute('src');
    video.src = objectUrl;
    video.pause();
    fitVideoToViewport();
}

function receivePacket(buffer) {
    if (buffer.byteLength < HEADER_BYTES) {
        return;
    }
    const view = new DataView(buffer);
    const magic = view.getUint32(0, false);
    if (magic === CMAF_MAGIC) {
        receiveVideoPacket(buffer, view);
        return;
    }
    if (magic === PCM_MAGIC) {
        receiveAudioPacket(buffer, view);
        return;
    }
    setStatus('\u6536\u5230\u975e\u6cd5\u5a92\u4f53\u5305', true);
}

function ensureAudioContext() {
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
        return;
    }
    if (!audioContext) {
        audioContext = new AudioContextClass({ latencyHint: 'playback' });
        audioNextTime = audioContext.currentTime + 0.15;
    }
    if (audioContext.state === 'suspended') {
        audioContext.resume().catch(() => {});
    }
}

function receiveAudioPacket(buffer, view) {
    if (!audioContext || buffer.byteLength < AUDIO_HEADER_BYTES) {
        return;
    }
    const version = view.getUint32(4, false);
    const sampleRate = view.getUint32(8, false);
    const channels = view.getUint32(12, false);
    const payloadBytes = view.getUint32(20, false);
    if (version !== 1 || channels < 1 || channels > 8 || AUDIO_HEADER_BYTES + payloadBytes > buffer.byteLength) {
        return;
    }

    const samples = new Float32Array(buffer, AUDIO_HEADER_BYTES, payloadBytes / Float32Array.BYTES_PER_ELEMENT);
    const frameCount = Math.floor(samples.length / channels);
    if (frameCount <= 0) {
        return;
    }
    const audioBuffer = audioContext.createBuffer(channels, frameCount, sampleRate);
    for (let channel = 0; channel < channels; channel++) {
        const target = audioBuffer.getChannelData(channel);
        for (let i = 0, source = channel; i < frameCount; i++, source += channels) {
            target[i] = samples[source];
        }
    }
    const source = audioContext.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(audioContext.destination);
    const now = audioContext.currentTime;
    if (audioNextTime < now + 0.05 || audioNextTime > now + 1.0) {
        audioNextTime = now + 0.12;
    }
    source.start(audioNextTime);
    audioNextTime += frameCount / sampleRate;
}

function receiveVideoPacket(buffer, view) {
    if (!('MediaSource' in window)) {
        setStatus('\u5f53\u524d\u6d4f\u89c8\u5668\u4e0d\u652f\u6301 MediaSource', true);
        return;
    }

    const version = view.getUint32(4, false);
    const flags = view.getUint32(8, false);
    const payloadBytes = view.getUint32(12, false);
    if (version !== 1 || HEADER_BYTES + payloadBytes !== buffer.byteLength) {
        setStatus('\u6536\u5230\u4e0d\u652f\u6301\u7684\u89c6\u9891\u5305', true);
        return;
    }

    const payload = new Uint8Array(buffer, HEADER_BYTES, payloadBytes);
    if ((flags & FLAG_INIT) !== 0) {
        initializeSourceBuffer(payload);
        return;
    }
    const keyframe = (flags & FLAG_KEYFRAME) !== 0;
    if (!initialized || (waitingForKeyframe && !keyframe)) {
        return;
    }
    if (keyframe) {
        waitingForKeyframe = false;
    }
    enqueueSegment(payload, keyframe);
    updateStats(width, height, sampleCountFromFragment(payload));
}

function initializeSourceBuffer(initSegment) {
    if (sourceBuffer) {
        return;
    }
    const codec = codecFromInitSegment(initSegment) || 'avc1.640028';
    const dimensions = dimensionsFromInitSegment(initSegment);
    width = dimensions.width || width;
    height = dimensions.height || height;
    fitVideoToViewport();
    const mime = `video/mp4; codecs="${codec}"`;
    if (!MediaSource.isTypeSupported(mime)) {
        setStatus(`\u6d4f\u89c8\u5668\u4e0d\u652f\u6301: ${mime}`, true);
        return;
    }
    const open = () => createSourceBuffer(mime, initSegment);
    if (mediaSource.readyState === 'open') {
        open();
    } else {
        mediaSource.addEventListener('sourceopen', open, { once: true });
    }
    initialized = true;
    waitingForKeyframe = false;
}

function createSourceBuffer(mime, init) {
    sourceBuffer = mediaSource.addSourceBuffer(mime);
    sourceBuffer.mode = 'segments';
    sourceBuffer.addEventListener('error', () => reconnectAfterPlaybackFailure('SourceBuffer error'));
    sourceBuffer.addEventListener('abort', () => reconnectAfterPlaybackFailure('SourceBuffer abort'));
    sourceBuffer.addEventListener('updateend', () => {
        trimBuffered();
        pumpAppendQueue();
    });
    enqueueSegment(init, true);
    video.pause();
}

function enqueueSegment(segment, keyframe) {
    if (appendQueueBytes + segment.length > MAX_APPEND_QUEUE_BYTES
            || appendQueue.length >= MAX_APPEND_QUEUE_SEGMENTS) {
        appendQueue = [];
        appendQueueBytes = 0;
        if (!keyframe) {
            waitingForKeyframe = true;
            setStatus('\u7f51\u7edc\u7f13\u51b2\u8fc7\u8f7d\uff0c\u7b49\u5f85\u5173\u952e\u5e27\u6062\u590d');
            return;
        }
        waitingForKeyframe = false;
    }
    appendQueue.push({ data: segment, keyframe });
    appendQueueBytes += segment.length;
    pumpAppendQueue();
}

function pumpAppendQueue() {
    if (!sourceBuffer || sourceBuffer.updating || appendQueue.length === 0) {
        return;
    }
    try {
        const next = appendQueue.shift();
        appendQueueBytes -= next.data.length;
        sourceBuffer.appendBuffer(next.data);
    } catch (error) {
        appendQueue = [];
        appendQueueBytes = 0;
        reconnectAfterPlaybackFailure(error.message || 'appendBuffer failed');
    }
}

function trimBuffered() {
    if (!sourceBuffer || sourceBuffer.updating || video.buffered.length === 0) {
        return;
    }
    const current = video.currentTime;
    const liveRange = latestBufferedRange();
    if (!liveRange) {
        return;
    }
    const start = liveRange.start;
    const end = liveRange.end;
    const bufferedAhead = end - current;
    if (waitingForStartupBuffer) {
        if (end - start < START_BUFFER_SECONDS) {
            return;
        }
        video.currentTime = Math.max(start, end - TARGET_LATENCY_SECONDS);
        waitingForStartupBuffer = false;
        video.playbackRate = 1.0;
        video.play().catch(() => {});
        return;
    }

    if (!liveRange.containsCurrent || bufferedAhead > PANIC_LATENCY_SECONDS) {
        video.currentTime = Math.max(start, end - TARGET_LATENCY_SECONDS);
        video.playbackRate = 1.0;
    } else if (bufferedAhead > MAX_LATENCY_SECONDS) {
        video.currentTime = Math.max(start, end - TARGET_LATENCY_SECONDS);
        video.playbackRate = 1.03;
    } else if (bufferedAhead > TARGET_LATENCY_SECONDS + 1.0) {
        video.playbackRate = 1.08;
    } else if (bufferedAhead > TARGET_LATENCY_SECONDS + 0.35) {
        video.playbackRate = 1.03;
    } else if (bufferedAhead < TARGET_LATENCY_SECONDS - 1.0) {
        video.playbackRate = 0.97;
    } else {
        video.playbackRate = 1.0;
    }
    if (video.paused) {
        video.play().catch(() => {});
    }
    if (video.buffered.length > 1) {
        try {
            sourceBuffer.remove(video.buffered.start(0), start);
        } catch {
            // Removing obsolete ranges is best-effort; playback can continue.
        }
    }
    if (current - start > BACK_BUFFER_SECONDS) {
        try {
            sourceBuffer.remove(start, current - BACK_BUFFER_SECONDS / 2);
        } catch {
            // Back-buffer trimming is opportunistic.
        }
    }
}

function updateStats(w, h, frameCount) {
    frames += Math.max(1, frameCount);
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
        const liveRange = latestBufferedRange();
        const liveBuffer = liveRange
                ? Math.max(0, liveRange.end - video.currentTime).toFixed(1)
                : '0.0';
        setStatus(`${w}x${h} | ${frames} FPS | buffer ${liveBuffer}s | video/MSE`);
        frames = 0;
        lastFpsTime = now;
    }
}

function latestBufferedRange() {
    if (video.buffered.length === 0) {
        return null;
    }
    const last = video.buffered.length - 1;
    const start = video.buffered.start(last);
    const end = video.buffered.end(last);
    const current = video.currentTime;
    return {
        start,
        end,
        containsCurrent: current >= start - 0.05 && current <= end + 0.05
    };
}

function fitVideoToViewport() {
    const viewportWidth = Math.max(0, viewport.clientWidth);
    const viewportHeight = Math.max(0, viewport.clientHeight);
    const sourceWidth = width || video.videoWidth || 16;
    const sourceHeight = height || video.videoHeight || 9;
    if (viewportWidth <= 0 || viewportHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0) {
        return;
    }

    // The viewer is a pure contain transform: preserve source aspect ratio,
    // choose the largest rectangle inside the available viewport, and never crop.
    const scale = Math.min(viewportWidth / sourceWidth, viewportHeight / sourceHeight);
    const fittedWidth = Math.max(1, Math.floor(sourceWidth * scale));
    const fittedHeight = Math.max(1, Math.floor(sourceHeight * scale));
    video.style.setProperty('--fit-width', `${fittedWidth}px`);
    video.style.setProperty('--fit-height', `${fittedHeight}px`);
}

async function toggleFullscreen() {
    try {
        if (document.fullscreenElement) {
            await document.exitFullscreen();
            return;
        }
        if (viewport.requestFullscreen) {
            await viewport.requestFullscreen({ navigationUI: 'hide' });
        } else if (video.webkitEnterFullscreen) {
            video.webkitEnterFullscreen();
        }
    } catch {
        setStatus('\u5168\u5c4f\u8bf7\u6c42\u88ab\u6d4f\u89c8\u5668\u62d2\u7edd', true);
    }
}

function updateFullscreenButton() {
    fullscreenButton.textContent = document.fullscreenElement ? '\u9000' : '\u5168';
    fullscreenButton.title = document.fullscreenElement ? '\u9000\u51fa\u5168\u5c4f' : '\u5168\u5c4f';
    fullscreenButton.setAttribute('aria-label', fullscreenButton.title);
    fitVideoToViewport();
}

function codecFromInitSegment(segment) {
    const offset = findBox(segment, 'avcC', 0, segment.length);
    if (offset < 0 || offset + 12 > segment.length) {
        return null;
    }
    return 'avc1.' + hex2(segment[offset + 9]) + hex2(segment[offset + 10]) + hex2(segment[offset + 11]);
}

function dimensionsFromInitSegment(segment) {
    const offset = findBox(segment, 'tkhd', 0, segment.length);
    if (offset < 0 || offset + 92 > segment.length) {
        return { width: 0, height: 0 };
    }
    return {
        width: readUint32(segment, offset + 84) >>> 16,
        height: readUint32(segment, offset + 88) >>> 16
    };
}

function sampleCountFromFragment(fragment) {
    const trun = findBox(fragment, 'trun', 0, fragment.length);
    if (trun < 0 || trun + 16 > fragment.length) {
        return 1;
    }
    return readUint32(fragment, trun + 12);
}

function findBox(data, type, start, end) {
    const bytes = new TextEncoder().encode(type);
    let offset = start;
    while (offset + 8 <= end) {
        const size = readUint32(data, offset);
        if (size < 8 || offset + size > end) {
            return -1;
        }
        if (data[offset + 4] === bytes[0] && data[offset + 5] === bytes[1]
                && data[offset + 6] === bytes[2] && data[offset + 7] === bytes[3]) {
            return offset;
        }
        const child = findBox(data, type, offset + 8, offset + size);
        if (child >= 0) {
            return child;
        }
        offset += size;
    }
    return -1;
}

function readUint32(data, offset) {
    return ((data[offset] << 24) >>> 0) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
}

function hex2(value) {
    return value.toString(16).padStart(2, '0');
}

connectButton.addEventListener('click', connect);
fullscreenButton.addEventListener('click', toggleFullscreen);
document.addEventListener('fullscreenchange', updateFullscreenButton);
window.addEventListener('resize', fitVideoToViewport);
window.addEventListener('orientationchange', () => setTimeout(fitVideoToViewport, 250));
video.addEventListener('loadedmetadata', fitVideoToViewport);
if ('ResizeObserver' in window) {
    resizeObserver = new ResizeObserver(fitVideoToViewport);
    resizeObserver.observe(viewport);
}
passwordInput.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
        connect();
    }
});
fitVideoToViewport();
