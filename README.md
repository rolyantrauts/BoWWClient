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
  -t <string>    KWS Params: Threshold,Decay,WindowSec (default: 0.75,0.1,0.6)
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
You can have multiple clients in a group so you can have multiple mics in a zone.  
The mics that pick up a wakeword transmit the wakeword threshold and best threshold is chosen as best stream  

-d <device>    ALSA KWS Mono Input (default: plughw:Loopback,1,0)  
This is the 16khz 16bit mono audio stream for the KWS  
-A <device>    ALSA Multi-Mic Array Input (Streaming Source)  
If not defined the -d <device> stream will be streamed to the server.  
If defined the wakeword will use -d <device> but stream the -A <device>

# Hardware
Any single mic or channel from 2/4 mic hat boards can be used.  
A recomendation is for the [MAX9814](https://learn.adafruit.com/adafruit-agc-electret-microphone-amplifier-max9814/overview)  
![max9814](https://cdn-learn.adafruit.com/assets/assets/000/014/304/small360/adafruit_products_1713_LRG.jpg?1392325003)  
The preamp and analogue agc of the max9814 helps much with farfield over standard mics and outputs a line voltage less sussceptable to noise for further distance.  
You can use as is, a 1uf film capacitor on the output with block dc noise, having a 3.3v LDO at the mic end of the cable will also reduce noise even further as the Pi5v is not the cleanest.  
Short VCC & gain for 40 dB gain leave AR floating.  
The CM108 board type USB soundcards are good, cheap and you know what your getting.  
Multiple BoWWClient can connect in groups to BoWWServer to create coverage for a room and due to AUC alg best stream is chosen for ASR

🎙️ Wake Word Envelope Tuning (Client-Side)
The BoWW Edge Client runs a highly efficient INT8 quantized TFLite model for low-latency, continuous listening. To prevent false positives while maintaining zero-latency responsiveness, the client shapes the neural network's raw output into an acoustic envelope using ADSR (Attack, Decay, Sustain/Release) mathematics.

You configure the envelope engine via the command line using the -t flag followed by a comma-separated string:
-t <Type>,<Threshold>,<Attack>,<Hold>,<Decay>

Parameters
Type (char): The verification algorithm to use (l for Leading, a for Average).

Threshold (float): The raw probability (0.0 to 1.0) the INT8 model must hit.

Attack (int): The consecutive number of frames the raw probability must exceed the threshold to arm the wake word state machine.

Hold (int): The size of the sliding window used to smooth the neural network output.

Decay (float): The drop in probability required to close the gate, finalize the word, and send the audio stream to the server.

Mode 1: Leading Edge (Relative Decay)
Best for: Quiet to moderate rooms, close-up microphones, or environments where you want instant, zero-latency responsiveness.
Logic: The client trusts the initial raw "Attack" completely. Once the gate opens, it dynamically tracks the highest point the smoothed average ever reaches. The gate closes (and the stream is sent to the server) the moment the average drops by your Decay amount relative to that peak. It closely hugs the shape of the word.

Example Command:
```
./BoWWClient -d plughw:0,0 -m models/hey_jarvis_int8.tflite -t l,0.90,4,20,0.20
```
(Triggers instantly on 4 frames of 90% confidence. Uses a 20-frame smoothing window. Closes the gate when the average drops 20% from its highest peak).

Mode 2: Average (Absolute Decay)
Best for: Noisy environments (living rooms with a TV on) where transient background noise causes the INT8 model to spike unpredictably.
Logic: The client hears the fast raw "Attack" but refuses to validate the wake word until the smoothed average also proves it has enough sustained acoustic energy to cross the threshold. Once validated, it waits for the average to drop below a fixed, absolute hard line (Threshold - Decay) before sending the stream to the server.

Example Command:
```
./BoWWClient -d plughw:0,0 -m models/hey_jarvis_int8.tflite -t a,0.85,5,30,0.15
```
(Arms on 5 frames of 85% confidence, but demands the 30-frame running average also crosses 85%. Closes the gate only when the average drops strictly below 0.70).

