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
Still to create a BT mobile app as local BT is sceure by nature.

On the server when 1st enrolling a client it gains a temp_id
A mobile app will add that temp_id and guid_id to add that client to a group.
Currently will have to be done by hand client_guid.txt containing.
`871fb1f9-7ecf-426e-a5ce-a6ad67cfdd72`

Server
clients.yaml
```
groups:
  - name: "bedroom"
    sample_rate: 16000
    channels: 1
    arbitration_timeout_ms: 200
    vad_threshold: 0.75
    vad_no_voice_ms: 2000
    output: "file"       # C++ expects string: "file" or "alsa"
    device: ""           # Only used if output is "alsa" (e.g., "hw:0,0")

clients:
  - guid: "871fb1f9-7ecf-426e-a5ce-a6ad67cfdd72"
    group: "bedroom"
    # Update this TempID to whatever the server prints in the log when you connect
```


