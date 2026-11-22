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
For AirPlay 2 senders that yield in PTP clock selection, the receiver also
serves its local clock by unicast to the authenticated session peer.
RTSP discovery and auxiliary control connections can coexist with an active
stream. Audio setup alone takes ownership of shared playback and timing.
An authenticated AirPlay 2 session with negotiated timing can acknowledge
AirFlash's early RECORD before the stream SETUP. When the authenticated peer
provides the PTP clock, the receiver sends unicast Delay_Req messages to that
peer so senders that wait for a timing exchange can start streaming.
For realtime streams that follow the early-RECORD sequence, the receiver
negotiates a 50 ms minimum playout latency if the sender requests less and
caps startup preroll at half of the stream's playout latency.
