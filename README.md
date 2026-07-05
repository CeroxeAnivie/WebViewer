# WebViewer

WebViewer 是一个面向 Windows 的高质量屏幕直播工具。它在本机启动一个轻量级单端口 Web 服务，浏览器打开页面后即可观看实时屏幕画面。

项目目标很明确：把屏幕采集、编码、传输和播放都放到适合的位置处理。Java 负责配置、HTTP/WebSocket 服务和进程生命周期；Windows 原生后端负责 D3D11/WGC/DXGI 采集、NVENC 编码与系统声音采集；前端只作为普通视频播放器接收 fMP4/CMAF live stream，避免逐帧 JavaScript 解码或重封装。

## 特性

- 轻量级 Java 入口，无 Spring Boot。
- 单端口服务：HTTP 页面和 WebSocket 媒体流共用同一个端口，方便内网穿透。
- Windows 原生采集后端，优先走 WGC/D3D11/NVENC 路径。
- 内置 WASAPI loopback 系统声音采集。
- H.264 fMP4/CMAF 直播片段，前端使用浏览器原生 MediaSource 播放。
- 播放端采用直播稳态缓冲策略，优先保证清晰、连续、流畅。
- 页面按可用窗口等比例 contain 缩放，禁止裁切画面。
- 支持交互式中文配置，也支持命令行参数直接启动。
- 原生 DLL 自动打包进 jar，运行时自动释放加载。

## 环境要求

- Windows 10/11 x64
- Java 21
- Maven Wrapper 已随项目提供
- Visual Studio C++ Build Tools 或 Visual Studio 2022 C++ 工具链
- 支持 D3D11 桌面采集的显卡与驱动
- 推荐 NVIDIA 显卡以使用 NVENC

## 构建

使用项目内置 Maven Wrapper：

```cmd
chcp 65001 >nul
mvnw.cmd clean package
```

构建成功后产物位于：

```text
target\WebViewer-2.0.1.jar
```

jar 内会包含：

```text
native/windows-x64/webviewer_capture.dll
static/index.html
static/app.js
top/ceroxe/webviewer/App.class
```

## 运行

交互式运行：

```cmd
chcp 65001 >nul
java -jar target\WebViewer-2.0.1.jar
```

命令行运行示例：

```cmd
chcp 65001 >nul
java -jar target\WebViewer-2.0.1.jar --http-port=8080 --width=2560 --height=1600 --fps=120 --password=1122
```

启动后访问：

```text
http://localhost:8080
```

局域网或穿透访问时使用实际主机地址和端口。

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--host` | `0.0.0.0` | HTTP/WebSocket 监听地址 |
| `--http-port` | `8080` | 页面与媒体流共用端口 |
| `--ws-port` | 同 `--http-port` | 兼容参数，默认同端口 |
| `--width` | `1920` | 输出宽度 |
| `--height` | `1080` | 输出高度 |
| `--fps` | `60` | 输出帧率，范围 1-240 |
| `--bitrate` | 自动推荐 | 视频码率，单位 bps |
| `--display` | `0` | 显示器索引 |
| `--gpu` | `-1` | 采集 GPU 索引，`-1` 表示自动 |
| `--password` | 无 | 访问密码，必填 |

不传 `--bitrate` 时，程序会根据分辨率和帧率自动推荐直播码率。推荐值按网页直播的持续吞吐模型计算，高帧率不会简单线性翻倍码率；如果只在内网或专线环境使用，可以手动提高 `--bitrate`。

## 播放模型

WebViewer 的播放链路按直播播放器模型设计：

1. 原生后端采集屏幕与系统声音。
2. NVENC 或系统编码器输出 H.264 Annex B。
3. Java 侧封装为 fMP4/CMAF live segment。
4. WebSocket 发送二进制媒体包。
5. 前端使用 MediaSource 顺序 append 片段。
6. 浏览器原生视频管线负责解码、缓冲、渲染和全屏。

这个模型牺牲极低延迟，换取直播软件更重要的稳定性、清晰度和连续播放体验。

## 网络说明

WebViewer 使用一个 TCP 端口承载页面和媒体流：

- 普通 HTTP 请求返回前端页面与静态资源。
- `Upgrade: websocket` 的 `/stream` 请求进入媒体流。

因此做内网穿透时只需要转发 `--http-port` 指定的端口。穿透工具必须完整支持 WebSocket 长连接和大带宽持续传输。

## 画面缩放

前端永远按 contain 模式显示画面：

- 保持采集源长宽比。
- 使用窗口内可容纳的最大矩形。
- 允许上下或左右黑边。
- 不拉伸、不裁切、不覆盖工具栏。

## 编码与乱码

Windows 终端建议先执行：

```cmd
chcp 65001 >nul
```

如果终端仍使用 ANSI/GBK，可以通过 JVM 参数指定控制台编码：

```cmd
java -Dwebviewer.consoleCharset=GBK -jar target\WebViewer-2.0.1.jar
```

## 开发约定

- Java 包名：`top.ceroxe.webviewer`
- Maven 坐标：`top.ceroxe:WebViewer`
- 主类：`top.ceroxe.webviewer.App`
- 原生库：`webviewer_capture.dll`
- 前端入口：`src/main/resources/static/index.html`
