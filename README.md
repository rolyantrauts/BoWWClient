# BoWWClient
Wakeword client for BoWWServer

`./BoWWClient -d plughw:3 -m ../models/hey_jarvis_int8.tflite -t 0.75 -D`  
`./boww_server --debug`
```
BoWWClient - Edge Smart Speaker Engine
Usage: ./BoWWClient [OPTIONS]

Options:
  -c <dir>       Path to config dir for client_guid.txt (default: ./)
  -d <device>    ALSA KWS Mono Input (default: plughw:Loopback,1,0)
  -A <device>    ALSA Multi-Mic Array Input (Streaming Source)
  -s <uri>       Manual Server URI override (e.g., ws://192.168.1.50:9002)
  -p <float>     Pre-roll buffer duration in seconds (default: 3.0)
  -m <filepath>  Path to trained .tflite model file
  -t <float>     Envelope threshold 0.0 to 1.0 (default: 0.75)
  -D             Enable Debug Mode (Live VU and logs)
  -h             Show this help message and exit
```
