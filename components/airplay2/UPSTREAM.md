# AirPlay 2 receiver

The protocol, decoding and playback scheduler are taken from
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) at commit
`35be6fcb58f7224a50b9f8500482fa8a2a989307`. The original notices and license
files are included here. The integration provides this firmware's NVS
namespace, network identity and USB Host PCM output. The mDNS initializer
reports initialization errors to the application. The RTSP server registers
the L16 codec and starts NTP timing for senders that select it in AirPlay 2
SETUP. The NTP client samples more frequently until it locks. RTSP reports
the effective playback volume from the application's USB output settings.
