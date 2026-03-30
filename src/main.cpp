#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>
#include <csignal>
#include <algorithm>
#include <fstream>
#include <thread>
#include <chrono>
#include <deque>
#include <mutex>
#include <sstream>
#include <getopt.h>
#include <alsa/asoundlib.h>

#include "feature_extract.h"
#include "tflite_runner.h"
#include "AudioRingBuffer.h"
#include "WebSocketClient.h"

// --- Global State Machine ---
enum AppState { LISTENING, STREAMING };
std::atomic<AppState> g_current_state{LISTENING};
std::atomic<bool> g_running{true};
std::atomic<bool> g_authenticated{false}; 

std::mutex g_network_mtx;
std::string g_client_guid = "";

void signal_handler(int signum) {
    std::cout << "\n[!] Caught shutdown signal. Closing pipelines gracefully...\n";
    g_running = false;
}

void print_usage(const char* prog_name) {
    std::cout << "\nBoWWClient - Edge Smart Speaker Engine\n"
              << "Usage: " << prog_name << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c <dir>       Path to config dir for client_guid.txt (default: ./)\n"
              << "  -D <device>    ALSA KWS Mono Input (default: plughw:Loopback,1,0)\n"
              << "  -A <device>    ALSA Multi-Mic Array Input (Streaming Source)\n"
              << "  -s <uri>       Manual Server URI override (e.g., ws://192.168.1.50:9002)\n"
              << "  -m <filepath>  Path to trained .tflite model file\n"
              << "  -t <string>    KWS Params: Type,Threshold,Attack,Hold,Decay (default: l,0.9,4,20,0.2)\n"
              << "                 Types: 'l' (Leading), 'a' (Average)\n"
              << "  -d             Enable Debug Mode (Live VU and logs)\n"
              << "  -h             Show this help message and exit\n\n";
}

void load_guid(const std::string& config_dir) {
    std::string path = config_dir + "client_guid.txt";
    std::ifstream file(path);
    if (file.is_open()) {
        std::getline(file, g_client_guid);
        g_authenticated = true;
        std::cout << "[Auth] Loaded existing GUID: " << g_client_guid << " from " << path << "\n";
    } else {
        std::cout << "[Auth] No GUID found. Device will run in quarantine mode until onboarded.\n";
    }
}

void save_guid(const std::string& config_dir, const std::string& guid) {
    std::string path = config_dir + "client_guid.txt";
    std::ofstream file(path);
    if (file.is_open()) {
        file << guid;
        g_client_guid = guid;
        g_authenticated = true;
        std::cout << "[Auth] Saved new GUID: " << guid << " to " << path << "\n";
    } else {
        std::cerr << "[!] Error: Could not save GUID to " << path << "\n";
    }
}

std::string discover_server_mdns() {
    std::cout << "[mDNS] Searching local network for BoWWServer (_boww._tcp)...\n";
    FILE* pipe = popen("avahi-browse -rtp _boww._tcp 2>/dev/null", "r");
    if (!pipe) return "";
    
    char buffer[256];
    std::string ws_uri = "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        std::string line(buffer);
        if (line.rfind("=", 0) == 0) { 
            std::vector<std::string> tokens;
            size_t start = 0, end = 0;
            while ((end = line.find(';', start)) != std::string::npos) {
                tokens.push_back(line.substr(start, end - start));
                start = end + 1;
            }
            tokens.push_back(line.substr(start));

            if (tokens.size() >= 9 && tokens[2] == "IPv4") {
                std::string ip = tokens[7];
                std::string port = tokens[8];

                ws_uri = "ws://" + ip + ":" + port;
                std::cout << "[mDNS] Resolved Server: " << ws_uri << "\n";
                break; 
            }
        }
    }
    pclose(pipe);
    return ws_uri;
}

class LookaheadAGC {
private:
    std::deque<std::vector<float>> delay_line_;
    float current_gain_;
    float target_peak_;
    float max_gain_;
    float attack_coeff_;
    float release_coeff_;

public:
    LookaheadAGC(int chunks=5, float target=0.80f, float max_g=15.0f, float att=0.05f, float rel=1.5f, float sr=16000.0f)
        : current_gain_(1.0f), target_peak_(target), max_gain_(max_g) {
        for (int i = 0; i < chunks; ++i) delay_line_.push_back(std::vector<float>(320, 0.0f));
        attack_coeff_ = std::exp(-1.0f / (att * sr));
        release_coeff_ = std::exp(-1.0f / (rel * sr));
    }

    void process(const std::vector<float>& in, std::vector<float>& out) {
        delay_line_.push_back(in);
        float max_peak = 0.0001f; 
        for (const auto& chunk : delay_line_) {
            for (float val : chunk) {
                if (std::abs(val) > max_peak) max_peak = std::abs(val);
            }
        }
        float target_gain = std::min(target_peak_ / max_peak, max_gain_);
        out = delay_line_.front();
        delay_line_.pop_front();

        for (size_t i = 0; i < out.size(); ++i) {
            if (target_gain < current_gain_) current_gain_ = attack_coeff_ * current_gain_ + (1.0f - attack_coeff_) * target_gain;
            else current_gain_ = release_coeff_ * current_gain_ + (1.0f - release_coeff_) * target_gain;
            out[i] = std::clamp(out[i] * current_gain_, -1.0f, 1.0f);
        }
    }
    float get_current_gain() const { return current_gain_; }
};

class WindowAverager {
private:
    std::vector<float> window_;
    int head_;
    float current_sum_;

public:
    WindowAverager(int size) : window_(size, 0.0f), head_(0), current_sum_(0.0f) {}

    void process(float new_prob, float& out_smoothed_prob) {
        current_sum_ -= window_[head_];
        window_[head_] = new_prob;
        current_sum_ += new_prob;
        head_ = (head_ + 1) % window_.size();

        out_smoothed_prob = current_sum_ / static_cast<float>(window_.size());
    }

    float get_current_sum() const { return current_sum_; }

    void reset() {
        std::fill(window_.begin(), window_.end(), 0.0f);
        current_sum_ = 0.0f;
    }
};

snd_pcm_t* init_alsa_mono(const std::string& device, int rate, snd_pcm_uframes_t period) {
    snd_pcm_t* handle;
    if (snd_pcm_open(&handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) return nullptr;
    
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle, hw);
    snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, hw, SND_PCM_FORMAT_S16_LE); 
    snd_pcm_hw_params_set_channels(handle, hw, 1);                   
    
    unsigned int r = rate;
    snd_pcm_hw_params_set_rate_near(handle, hw, &r, 0);
    snd_pcm_hw_params_set_period_size_near(handle, hw, &period, 0);
    
    snd_pcm_uframes_t buffer_size = period * 8; 
    snd_pcm_hw_params_set_buffer_size_near(handle, hw, &buffer_size);
    snd_pcm_hw_params(handle, hw);
    snd_pcm_prepare(handle);
    return handle;
}

void array_capture_loop(std::string device, int rate, int hop, AudioRingBuffer* buffer, WebSocketClient* ws) {
    snd_pcm_t* mic = init_alsa_mono(device, rate, hop);
    if (!mic) {
        std::cerr << "[!] FATAL: Failed to open Array device " << device << "\n";
        return;
    }

    std::vector<int16_t> audio_buf(hop);
    while (g_running) {
        int frames = snd_pcm_readi(mic, audio_buf.data(), hop);
        if (frames < 0) { snd_pcm_recover(mic, frames, 1); continue; }

        if (g_current_state == LISTENING) {
            buffer->push(audio_buf);
        } else if (g_current_state == STREAMING) {
            std::lock_guard<std::mutex> lock(g_network_mtx);
            ws->send_audio(audio_buf);
        }
    }
    snd_pcm_close(mic);
}

enum KWS_State { IDLE, ARMED };

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_dir = "./";
    std::string alsa_kws_dev = "plughw:Loopback,1,0";
    std::string alsa_array_dev = "";
    std::string manual_uri = "";
    std::string model_file = "models/wakeword.tflite";
    
    // --- DEFAULT ADSR PARAMETERS ---
    char kws_mode = 'l'; // 'l' = Leading, 'a' = Average
    float kws_threshold = 0.90f;
    int kws_attack = 4;
    int kws_hold = 20;
    float kws_decay = 0.20f;
    bool debug_mode = false;

    int opt;
    // Note: Swapped 'd' and 'D' in the getopt string
    while ((opt = getopt(argc, argv, "c:D:A:s:t:m:dh")) != -1) {
        switch (opt) {
            case 'c': 
                config_dir = optarg; 
                if (!config_dir.empty() && config_dir.back() != '/' && config_dir.back() != '\\') {
                    config_dir += "/";
                }
                break;
            case 'D': alsa_kws_dev = optarg; break; // Swapped to -D for device
            case 'A': alsa_array_dev = optarg; break;
            case 's': manual_uri = optarg; break;
            case 't': {
                std::string arg(optarg);
                std::stringstream ss(arg);
                std::string token;
                std::vector<std::string> tokens;
                
                // Split by comma
                while (std::getline(ss, token, ',')) {
                    tokens.push_back(token);
                }
                
                if (!tokens.empty()) {
                    int idx = 0;
                    
                    // Check if the user provided the explicit 'a' or 'l' mode prefix
                    if (tokens[0] == "a" || tokens[0] == "l") {
                        kws_mode = tokens[0][0];
                        idx++;
                    }
                    
                    // Safely parse the remaining float/int parameters
                    try {
                        if (idx < tokens.size()) kws_threshold = std::stof(tokens[idx++]);
                        if (idx < tokens.size()) kws_attack = std::stoi(tokens[idx++]);
                        if (idx < tokens.size()) kws_hold = std::stoi(tokens[idx++]);
                        if (idx < tokens.size()) kws_decay = std::stof(tokens[idx++]);
                    } catch (...) {
                        std::cerr << "[!] Error parsing -t parameters. Using defaults.\n";
                    }
                }
                break;
            }
            case 'm': model_file = optarg; break;
            case 'd': debug_mode = true; break; // Swapped to -d for debug
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    // --- Prevent Segfaults from bad user inputs ---
    kws_hold = std::max(1, kws_hold); 

    const int SAMPLE_RATE = 16000;
    const int HOP_STEP = 320;          
    const int TF_FRAME_LENGTH = 640;   
    const int TF_MFCC_BINS = 20;       

    std::cout << "\n[OK] BoWWClient Edge Node Online.\n";
    std::cout << "[KWS] Params -> Mode: " << (kws_mode == 'a' ? "Average" : "Leading")
              << " | Threshold: " << kws_threshold 
              << " | Attack: " << kws_attack << " frames"
              << " | Hold: " << kws_hold << " frames"
              << " | Decay: " << kws_decay << "\n";
              
    load_guid(config_dir);
    
    WebSocketClient ws_client;
    std::string ws_uri = manual_uri.empty() ? discover_server_mdns() : manual_uri;
    
    if (ws_uri.empty()) {
        std::cerr << "[!] Server not found via mDNS. Provide -s <uri> override.\n";
        return 1;
    }
    
    AudioRingBuffer pre_roll_buffer(SAMPLE_RATE * 1.0f);
    
    std::cout << "[Network] Connecting to " << ws_uri << "...\n";
    
    ws_client.on_connected = [&]() {
        std::lock_guard<std::mutex> lock(g_network_mtx);
        if (g_authenticated) {
            std::cout << "[Network] Authenticated as " << g_client_guid << "\n";
            ws_client.send_hello(g_client_guid);
        } else {
            std::cout << "[Network] Unenrolled. Requesting Temp ID from server...\n";
            ws_client.send_enroll();
        }
    };
    
    ws_client.on_disconnected = [&]() {
        std::cerr << "\n[!] Connection to server lost. Shutting down gracefully...\n";
        g_running = false; 
    };

    ws_client.on_hello_ack = [&](float preroll_seconds) {
        std::cout << "[Network] Handshake complete. Syncing pre-roll buffer to " << preroll_seconds << "s.\n";
        pre_roll_buffer.resize(static_cast<size_t>(SAMPLE_RATE * preroll_seconds));
    };

    ws_client.on_temp_id_assigned = [&](std::string temp_id) {
        std::cout << "\n======================================================\n";
        std::cout << " [Auth] Server assigned Temp ID: " << temp_id << "\n";
        std::cout << " [Auth] Ready for Bluetooth App Onboarding...\n";
        std::cout << "======================================================\n\n";
    };
    
    ws_client.on_guid_assigned = [&](std::string new_guid) {
        std::cout << "\n[Auth] Received onboarding payload from Server!\n";
        save_guid(config_dir, new_guid);
        
        std::lock_guard<std::mutex> lock(g_network_mtx);
        ws_client.send_hello(g_client_guid);
    };

    TFLiteRunner wakeword_model(model_file);
    WindowAverager jarvis_averager(kws_hold);

    KWS_State current_kws_state = IDLE;
    int attack_counter = 0;
    
    float smoothed_peak = 0.0f;
    float raw_peak = 0.0f;
    float current_auc = 0.0f;
    int current_frame_count = 0; 
    
    std::vector<float> attack_raw_history;
    std::vector<float> attack_avg_history;

    ws_client.on_start_command = [&]() {
        if (debug_mode) std::cout << "\n✅ [Server] Won arbitration! Streaming audio...\n";
    };

    ws_client.on_stop_command = [&]() {
        g_current_state = LISTENING;
        current_kws_state = IDLE;
        pre_roll_buffer.flush(); 
        wakeword_model.reset_states();
        jarvis_averager.reset();
        attack_counter = 0;
        current_frame_count = 0; 
        if (debug_mode) std::cout << "\n[Server] Stop Command Rx'd. Returning to Listen.\n\n";
    };

    if (!ws_client.connect(ws_uri)) {
        std::cerr << "[!] Could not establish connection to BoWWServer. Exiting.\n";
        return 1;
    }

    std::thread array_thread;
    if (!alsa_array_dev.empty()) {
        array_thread = std::thread(array_capture_loop, alsa_array_dev, SAMPLE_RATE, HOP_STEP, &pre_roll_buffer, &ws_client);
    }

    snd_pcm_t* kws_mic = init_alsa_mono(alsa_kws_dev, SAMPLE_RATE, HOP_STEP);
    if (!kws_mic) {
        std::cerr << "\n[!] FATAL: Could not open ALSA microphone: " << alsa_kws_dev << "\n";
        std::cerr << "[!] Is another instance running? Try: sudo killall -9 BoWWClient\n\n";
        return 1;
    }
    
    LookaheadAGC agc(5, 0.80f, 15.0f, 0.05f, 1.5f, SAMPLE_RATE);
    FeatureExtractor feature_extractor;

    std::vector<int16_t> audio_buf(HOP_STEP);
    std::vector<float> hop_float(HOP_STEP);
    std::vector<float> agc_float(HOP_STEP);
    std::vector<float> sliding_audio_window(TF_FRAME_LENGTH, 0.0f); 
    std::vector<float> clean_buffer(TF_FRAME_LENGTH, 0.0f);
    std::vector<float> current_mfccs(TF_MFCC_BINS, 0.0f);

    std::cout << "[OK] Wake Word Engine Active. Listening...\n\n";

    while (g_running) {
        int frames_read = snd_pcm_readi(kws_mic, audio_buf.data(), HOP_STEP);
        if (frames_read < 0) { snd_pcm_recover(kws_mic, frames_read, 1); continue; }

        if (alsa_array_dev.empty()) {
            if (g_current_state == LISTENING) {
                pre_roll_buffer.push(audio_buf);
            } else if (g_current_state == STREAMING) {
                std::lock_guard<std::mutex> lock(g_network_mtx);
                ws_client.send_audio(audio_buf);
            }
        }

        for (int i = 0; i < HOP_STEP; ++i) hop_float[i] = static_cast<float>(audio_buf[i]) / 32768.0f;
        agc.process(hop_float, agc_float);

        std::memmove(sliding_audio_window.data(), sliding_audio_window.data() + HOP_STEP, (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
        std::memcpy(sliding_audio_window.data() + (TF_FRAME_LENGTH - HOP_STEP), agc_float.data(), HOP_STEP * sizeof(float));

        float sum = 0.0f;
        for (int i = 0; i < TF_FRAME_LENGTH; ++i) sum += sliding_audio_window[i];
        float mean = sum / static_cast<float>(TF_FRAME_LENGTH);
        for (int i = 0; i < TF_FRAME_LENGTH; ++i) clean_buffer[i] = sliding_audio_window[i] - mean;

        if (g_current_state == STREAMING) continue;

        feature_extractor.compute_mfcc_features(clean_buffer, current_mfccs);
        std::vector<float> scores = wakeword_model.infer(current_mfccs);
        
        float raw_jarvis_prob = scores.empty() ? 0.0f : scores[0]; 
        float smoothed_jarvis_prob = 0.0f;
        
        jarvis_averager.process(raw_jarvis_prob, smoothed_jarvis_prob);

        if (debug_mode && g_current_state == LISTENING && current_kws_state == IDLE) {
            int bars = static_cast<int>(smoothed_jarvis_prob * 20.0f);
            std::cout << "\r[Gain: " << agc.get_current_gain() << "] [Env: " << std::string(std::min(bars, 20), '#') 
                      << std::string(20 - std::min(bars, 20), '-') << "] | Avg: " << smoothed_jarvis_prob << "   " << std::flush;
        }

        if (current_kws_state == IDLE) {
            if (raw_jarvis_prob >= kws_threshold) {
                if (attack_counter == 0) {
                    current_auc = 0.0f;
                    raw_peak = 0.0f;
                    attack_raw_history.clear();
                    attack_avg_history.clear();
                }
                
                attack_counter++;
                current_auc += raw_jarvis_prob;
                
                attack_raw_history.push_back(raw_jarvis_prob);
                attack_avg_history.push_back(smoothed_jarvis_prob);
                
                if (raw_jarvis_prob > raw_peak) raw_peak = raw_jarvis_prob;
                
                if (attack_counter >= kws_attack) {
                    current_kws_state = ARMED;
                    smoothed_peak = *std::max_element(attack_avg_history.begin(), attack_avg_history.end());
                    current_frame_count = attack_counter; 
                    
                    if (debug_mode) {
                        std::cout << "\n\n[KWS] Armed! (Fast Attack via RAW over " << kws_attack << " frames)\n";
                        for (size_t i = 0; i < attack_raw_history.size(); ++i) {
                            std::cout << "[RAW] " << attack_raw_history[i] << " | [AVG] " << attack_avg_history[i] << " (Attack Frame " << (i+1) << ")\n";
                        }
                    }
                }
            } else {
                attack_counter = 0;
                current_auc = 0.0f;
            }
        } 
        else if (current_kws_state == ARMED) {
            current_auc += raw_jarvis_prob;
            current_frame_count++; 
            
            if (debug_mode) std::cout << "[RAW] " << raw_jarvis_prob << " | [AVG] " << smoothed_jarvis_prob << "\n";
            
            if (raw_jarvis_prob > raw_peak) raw_peak = raw_jarvis_prob;
            if (smoothed_jarvis_prob > smoothed_peak) smoothed_peak = smoothed_jarvis_prob;
            
            // --- DUAL-MODE RELEASE LOGIC ---
            bool should_release = false;
            
            if (kws_mode == 'l') {
                // Leading Mode: Relative Decay from peak
                float release_threshold = std::max(0.05f, smoothed_peak - kws_decay);
                if (smoothed_jarvis_prob <= release_threshold) should_release = true;
                
            } else if (kws_mode == 'a') {
                // Average Mode: Wait for the threshold, then absolute decay
                if (smoothed_peak >= kws_threshold) {
                    // Success Path: It crossed the threshold. Wait for it to fall back down.
                    if (smoothed_jarvis_prob < (kws_threshold - kws_decay)) {
                        should_release = true;
                    }
                } else {
                    // Waiting Path: Give the buffer up to 'Hold' frames to reach the threshold
                    if (current_frame_count >= kws_hold) {
                        should_release = true; // Buffer filled, but never hit threshold. Abort.
                    }
                }
            }

            if (should_release) {
                // --- VALIDATE EVENT ---
                bool is_valid = false;
                if (kws_mode == 'l') {
                    is_valid = true; // Leading mode always accepts the initial fast attack
                } else if (kws_mode == 'a' && smoothed_peak >= kws_threshold) {
                    is_valid = true; // Average mode demands the running average crossed the threshold
                }
                
                if (is_valid) {
                    if (!g_authenticated) {
                        if (debug_mode) std::cout << "\n[!] Wake word hit, but device is unenrolled. Ignoring.\n";
                    } else {
                        if (debug_mode) std::cout << "\n🔔 WAKE WORD COMPLETE! Raw Peak: " << raw_peak << " | Avg Peak: " << smoothed_peak << " | AUC Score: " << current_auc << " | Frames: " << current_frame_count << "\n\n";
                        
                        std::lock_guard<std::mutex> lock(g_network_mtx);
                        ws_client.send_confidence(current_auc, current_frame_count); 
                        ws_client.send_audio(pre_roll_buffer.flush());
                        g_current_state = STREAMING; 
                    }
                } else {
                    if (debug_mode) std::cout << "\n[KWS] Discarded: Average peak (" << smoothed_peak << ") never reached threshold (" << kws_threshold << ").\n\n";
                }
                
                current_kws_state = IDLE;
                attack_counter = 0;
                wakeword_model.reset_states();
                jarvis_averager.reset();
                current_frame_count = 0; 
            }
        }
    }

    if (kws_mic) snd_pcm_close(kws_mic);
    if (array_thread.joinable()) array_thread.join();
    return 0;
}
