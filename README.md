# S3 Audio

运行在 ESP32-S3 上的 Wi-Fi 音频接收器，由板载 USB OTG 以主机模式连接 USB DAC。项目使用 ESP-IDF v6.1 构建，默认配置适用于配有 16 MB Flash、8 MB Octal PSRAM 板卡。

## 使用

1. DAC 接开发板的原生 USB OTG 口，烧录和串口日志用独立的 UART/USB 转串口连接。OTG 接口需要能在主机模式下给 DAC 提供 5 V VBUS。
2. 连接热点 `S3-Audio-XXXXXX`，密码 `S3audio-setup`，在浏览器打开 `http://192.168.4.1/`。名称中的 XXXXXX 来自本板 Wi-Fi MAC 的末六位。
3. 在网页保存家里或工作室的 **2.4 GHz Wi-Fi**。板子重启，连上 Wi-Fi 并取得 IP 后会关闭配置热点。把手机或电脑接入同一局域网，访问串口日志显示的 `http://<设备IP>/`，或尝试 `http://s3-audio-xxxxxx.local/`。连不上已保存的 Wi-Fi 时，板子会继续开放配置热点。
4. 在 iPhone 或 iPad 的控制中心，从正在播放的音频输出列表选择 `S3 Audio XXXXXX`。播放视频时，画面仍在手机或平板上显示。DLNA 播放器也可从渲染器列表选择同一设备。

## 后台

- **USB DAC 输出**按 DAC 在全速 USB 模式下报告的双声道 PCM 能力提供采样率与位深组合。页面上方的数字为 USB 流实际启用的格式。选择自动时优先尝试 48 kHz / 16 bit，然后尝试其他可用组合。输出格式切换会短暂中断音频。
- **音量上限**限制 AirPlay 2 和 DLNA 送往 USB DAC 的 PCM 音量。默认 50%，可设为 0–100%，0% 静音。两种来源各自记忆上次音量，重启后恢复。开始播放时音量渐入，输出开始后的前 2.5 秒只接受降低音量的指令，避免投放端连接时自动调高。
- **GPIO48 WS2812**按一颗 GRB LED 配置。可选关闭、常亮或设备状态，并调整颜色和亮度。新增三种律动模式。状态模式下橙色闪烁代表配置热点，红色代表等待 DAC，绿色代表正在播放，其余时间使用自选颜色。
- **音量亮度**用所选主色随声音强弱变亮或变暗。**低频闪动**在低频突然增强时闪一下，主色可选。**三频变色**用红、绿、蓝分别显示低、中、高频。
- 三种律动读取实际送入 USB DAC 的 PCM，因此可用于 AirPlay 和 DLNA。停播后会渐暗。亮度滑块适用于全部模式。
- Wi-Fi、偏好的输出格式和灯光设置保存在 NVS 中。网页仅需本地局域网。若要通过本地 API 恢复配置热点，可向 `/api/wifi` 提交 `{"ssid":"","password":""}`，设备会重启。

## BOOT 恢复配置

设备启动后长按 BOOT（GPIO0）至少 5 秒，松手后自动重启。Wi-Fi、AirPlay 配对身份、音量记忆和上限、USB 输出格式及灯光恢复默认值，随后可连接设备的配置热点重新设置。

## 协议与输出范围

- AirPlay 2 在局域网通过 Bonjour 发布 `_airplay._tcp`。固件实现 HAP 配对、加密 RTSP、实时和缓冲音频接收，并由 USB Host 音频路径输出。多房间同步及各 iOS 应用的视频音频投送仍需在 Apple 设备上验证。
- WinAirCast 等可让接收端担任主时钟的电脑投放软件，在当前 AirPlay 2 会话中可以使用设备的单播 PTP 时钟。由发送端提供时钟的会话继续使用发送端时钟。
- AirPlay 发现、配对和辅助控制使用独立的 RTSP 连接。设备在新的音频流请求建立时切换播放会话。
- Windows AirFlash 可通过 AirPlay 2 投放。接收器支持 AirFlash 在配置音频流前发送的 `RECORD`，并在跟随发送端时钟时主动进行 PTP 延迟交换。如果这类实时连接请求低于 50 ms，接收器会在握手时返回 50 ms。AirFlash 的强制延迟会忽略协商值。
- DLNA 实现了 SSDP 发现、UPnP MediaRenderer 描述及 AVTransport、RenderingControl、ConnectionManager 控制。接收播放地址，由板子通过 HTTP/HTTPS 读取 MP3、WAV 或原生 FLAC，提供播放、暂停、停止、音量和静音。要切换曲目，由控制端提交下一个 URI。
- DAC 输出是双声道 PCM，通过 UAC1 或 UAC2。不同 CX31993 成品的 USB 描述符可能不同，能启用的格式以设备后台实际显示为准。输入和输出采样率不同时使用线性重采样。位深选项控制 USB 输出的数据格式。

## 重新构建和烧录

在工程目录的 ESP-IDF v6.1 PowerShell 中执行：

```powershell
idf.py build
idf.py -p COM7 flash
idf.py -p COM7 monitor
```

安装路径匹配时也可执行 `./build-local.ps1 -Action build` 或 `./build-local.ps1 -Action flash -Port COM7`。脚本可用 -Profile 指定 ESP-IDF v6.1 PowerShell 环境。将示例中的 COM7 换成开发板的串口。依赖版本在 `dependencies.lock` 固定。应用默认使用 ESP-IDF 的 Optimize for performance（GCC -O2）。首次构建需要访问 ESP Component Registry。工程内包含 AirPlay 2 接收、WAV、FLAC 和 MP3 解码的源文件。AirPlay 2 的解码器和密码库由锁定的 ESP-IDF 组件依赖提供。

`firmware/` 内包含同一次构建的三个镜像，可从该目录使用本机 ESP-IDF 环境直接烧录：

```powershell
python -m esptool --chip esp32s3 -p COM7 -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 s3_network_audio.bin
```

## USB 输出与灯光

GPIO48 连接一颗 GRB 顺序的 WS2812。原生 USB OTG 口继续作为主机接 CX31993，下载和监控走独立串口。固件分区保留此前的 NVS 位置，更新时使用正常烧录命令可以保留原有配网和输出偏好。

USB 写入任务与 USB Host 运行在 CPU0，DLNA 解码和 AirPlay 实时渲染在 CPU1。UAC2 使用较浅的 PCM 队列和 USB 缓冲。`/api/status` 提供最近采样的 `cpu0Load`、`cpu1Load`，以及 UAC2 播放时估算的 PCM 入队至 USB 总线延迟 `usbPathUs`。这个数值不包含发送端缓冲、网络传输或 DAC 的模拟输出延迟。

## 上游组件

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32/tree/35be6fcb58f7224a50b9f8500482fa8a2a989307) 提供 AirPlay 2 协议、配对和播放调度。上游说明及许可位于 `components/airplay2/`。
- [dr_libs](https://github.com/mackron/dr_libs/tree/e381981afa52800b9a3864df548353ff67252fbc)，用于流式 MP3、WAV、FLAC 解码。
- [Espressif UAC1 主机驱动](https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0) 和 [Avery Levitt UAC2 主机驱动](https://components.espressif.com/components/averyy/usb_host_uac2/versions/0.2.0)。
