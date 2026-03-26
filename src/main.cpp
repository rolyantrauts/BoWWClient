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
#include <getopt.h>
#include <alsa/asoundlib.h>

#include "feature_extract.h"
#include "tflite_runner.h"

// --- Global State ---
std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    std::cout << "\n[!] Caught shutdown signal. Closing audio pipeline gracefully...\n";
    g_running = false;
}

void print_usage(const char* prog_name) {
    std::cout << "\nBoWWClient - Bare-Metal WakeWord Engine\n"
              << "Usage: " << prog_name << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -d <device>    ALSA capture device (default: plughw:Loopback,1,0)\n"
              << "  -f <filepath>  Read from a 16kHz 16-bit Mono .wav file instead of ALSA\n"
              << "  -m <filepath>  Path to the trained .tflite model file\n"
              << "  -t <float>     Detection envelope threshold from 0.0 to 1.0 (default: 0.60)\n"
              << "  -D             Enable Debug Mode (outputs live VU meter and inference scores)\n"
              << "  -h             Show this help message and exit\n\n";
}

// ==============================================================================
// 1. Label Loader Helper
// ==============================================================================
std::vector<std::string> load_labels(const std::string& model_path) {
    std::string label_path = model_path;
    size_t ext_pos = label_path.rfind(".tflite");
    if (ext_pos != std::string::npos) {
        label_path.replace(ext_pos, 7, "_labels.txt");
    } else {
        label_path += "_labels.txt";
    }

    std::vector<std::string> labels;
    std::ifstream file(label_path);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty()) labels.push_back(line);
        }
        std::cout << "[OK] Loaded " << labels.size() << " labels from " << label_path << "\n";
    } else {
        std::cerr << "[!] WARNING: Could not open labels file: " << label_path << "\n";
        std::cerr << "             Output will default to raw indices.\n";
    }
    return labels;
}

// ==============================================================================
// 2. Lookahead AGC (Automatic Gain Control)
// ==============================================================================
class LookaheadAGC {
private:
    std::deque<std::vector<float>> delay_line_;
    float current_gain_;
    float target_peak_;
    float max_gain_;
    float attack_coeff_;
    float release_coeff_;

public:
    LookaheadAGC(int lookahead_chunks = 5,      // 5 chunks * 20ms = 100ms lookahead
                 float target_peak = 0.80f,     // The sweet spot for training
                 float max_gain = 15.0f,        // Maximum amplification for quiet rooms
                 float attack_sec = 0.05f,      // Fast drop to prevent clipping
                 float release_sec = 1.5f,      // Slow rise to bridge syllables
                 float sample_rate = 16000.0f)
        : current_gain_(1.0f), target_peak_(target_peak), max_gain_(max_gain) {
        
        // Pre-fill the delay line with silence so we can output immediately
        for (int i = 0; i < lookahead_chunks; ++i) {
            delay_line_.push_back(std::vector<float>(320, 0.0f));
        }

        // Calculate single-pole IIR smoothing coefficients
        attack_coeff_ = std::exp(-1.0f / (attack_sec * sample_rate));
        release_coeff_ = std::exp(-1.0f / (release_sec * sample_rate));
    }

    void process(const std::vector<float>& in_chunk, std::vector<float>& out_chunk) {
        delay_line_.push_back(in_chunk);

        // 1. Find absolute max peak in the future 100ms window
        float max_peak = 0.0001f; // Prevent divide by zero
        for (const auto& chunk : delay_line_) {
            for (float val : chunk) {
                float abs_val = std::abs(val);
                if (abs_val > max_peak) max_peak = abs_val;
            }
        }

        // 2. Calculate the raw target multiplier
        float target_gain = target_peak_ / max_peak;
        if (target_gain > max_gain_) {
            target_gain = max_gain_;
        }

        // 3. Grab the oldest chunk from the delay line
        out_chunk = delay_line_.front();
        delay_line_.pop_front();

        // 4. Apply sample-by-sample asymmetric smoothing
        for (size_t i = 0; i < out_chunk.size(); ++i) {
            if (target_gain < current_gain_) {
                // Fast Attack (sound got loud, drop gain quickly)
                current_gain_ = attack_coeff_ * current_gain_ + (1.0f - attack_coeff_) * target_gain;
            } else {
                // Slow Release (sound got quiet, raise gain slowly)
                current_gain_ = release_coeff_ * current_gain_ + (1.0f - release_coeff_) * target_gain;
            }
            
            out_chunk[i] *= current_gain_;
            
            // Hard soft-clip safety net
            if (out_chunk[i] > 1.0f) out_chunk[i] = 1.0f;
            if (out_chunk[i] < -1.0f) out_chunk[i] = -1.0f;
        }
    }
    
    float get_current_gain() const { return current_gain_; }
};

// ==============================================================================
// 3. Sliding Window Averager (O(1) Ring Buffer)
// ==============================================================================
class WindowAverager {
private:
    std::vector<float> window_;
    int head_;
    float current_sum_;
    int cooldown_timer_;
    int max_cooldown_;
    float threshold_;

public:
    WindowAverager(int window_size_frames, float threshold, int cooldown_frames)
        : window_(window_size_frames, 0.0f), head_(0), current_sum_(0.0f),
          cooldown_timer_(0), max_cooldown_(cooldown_frames), threshold_(threshold) {}

    bool process(float new_prob, float& out_smoothed_prob) {
        if (cooldown_timer_ > 0) {
            cooldown_timer_--;
        }

        current_sum_ -= window_[head_];
        window_[head_] = new_prob;
        current_sum_ += new_prob;
        head_ = (head_ + 1) % window_.size();

        out_smoothed_prob = current_sum_ / static_cast<float>(window_.size());

        if (cooldown_timer_ == 0 && out_smoothed_prob >= threshold_) {
            cooldown_timer_ = max_cooldown_; 
            return true;
        }
        return false;
    }

    void reset() {
        std::fill(window_.begin(), window_.end(), 0.0f);
        current_sum_ = 0.0f;
    }
};

// ==============================================================================
// 4. ALSA Hardware Initialization
// ==============================================================================
snd_pcm_t* init_alsa_mono(const std::string& device, int rate, snd_pcm_uframes_t period) {
    snd_pcm_t* handle;
    if (snd_pcm_open(&handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "[!] FATAL: Cannot open ALSA device: " << device << "\n";
        return nullptr;
    }

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

// ==============================================================================
// 5. Main WakeWord Loop 
// ==============================================================================
int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string alsa_dev = "plughw:Loopback,1,0";
    std::string model_file = "models/wakeword.tflite";
    std::string file_input = "";
    float threshold = 0.60f; 
    bool debug_mode = false;

    int opt;
    while ((opt = getopt(argc, argv, "d:f:t:m:Dh")) != -1) {
        switch (opt) {
            case 'd': alsa_dev = optarg; break;
            case 'f': file_input = optarg; break;
            case 't': threshold = std::stof(optarg); break;
            case 'm': model_file = optarg; break;
            case 'D': debug_mode = true; break;
            case 'h': 
                print_usage(argv[0]); 
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    const int SAMPLE_RATE = 16000;
    const int HOP_STEP = 320;          
    const int TF_FRAME_LENGTH = 640;   
    const int TF_MFCC_BINS = 20;       

    // --- TUNING PARAMETERS ---
    LookaheadAGC agc(5, 0.80f, 15.0f, 0.05f, 1.5f, SAMPLE_RATE);
    WindowAverager jarvis_averager(30, threshold, 40); // 600ms envelope, 800ms cooldown

    std::cout << "\n[OK] BoWWClient Online (Lookahead AGC + Envelope Mode).\n";
    if (file_input.empty()) {
        std::cout << "     ALSA Device: " << alsa_dev << "\n";
    } else {
        std::cout << "     File Input:  " << file_input << " (Simulated Real-Time)\n";
    }
    std::cout << "     Model File:  " << model_file << "\n";
    std::cout << "     Avg Threshold: " << threshold << " (over 600ms)\n";
    std::cout << "     Debug Mode:  " << (debug_mode ? "ON" : "OFF") << "\n\n";

    std::vector<std::string> class_labels = load_labels(model_file);
    TFLiteRunner wakeword_model(model_file);

    snd_pcm_t* mic = nullptr;
    std::ifstream wav_file;

    if (file_input.empty()) {
        mic = init_alsa_mono(alsa_dev, SAMPLE_RATE, HOP_STEP);
        if (!mic) return 1;
    } else {
        wav_file.open(file_input, std::ios::binary);
        if (!wav_file.is_open()) {
            std::cerr << "[!] FATAL: Could not open file: " << file_input << "\n";
            return 1;
        }
        wav_file.seekg(44, std::ios::beg); 
    }

    FeatureExtractor feature_extractor;

    std::vector<int16_t> audio_buf(HOP_STEP);
    std::vector<float> hop_float(HOP_STEP);
    std::vector<float> agc_float(HOP_STEP);
    std::vector<float> sliding_audio_window(TF_FRAME_LENGTH, 0.0f); 
    std::vector<float> clean_buffer(TF_FRAME_LENGTH, 0.0f);
    std::vector<float> current_mfccs(TF_MFCC_BINS, 0.0f);

    std::cout << "\n[OK] Processing Audio...\n\n";

    while (g_running) {
        if (!file_input.empty()) {
            wav_file.read(reinterpret_cast<char*>(audio_buf.data()), HOP_STEP * sizeof(int16_t));
            if (wav_file.gcount() == 0 || wav_file.eof()) {
                std::cout << "\n[OK] Reached end of audio file.\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
            int frames_read = snd_pcm_readi(mic, audio_buf.data(), HOP_STEP);
            if (frames_read < 0) { 
                snd_pcm_recover(mic, frames_read, 1); 
                continue; 
            }
        }

        // 1. Convert INT16 to Float [-1.0, 1.0]
        for (int i = 0; i < HOP_STEP; ++i) {
            hop_float[i] = static_cast<float>(audio_buf[i]) / 32768.0f;
        }

        // 2. Pass raw audio through the Lookahead AGC delay line
        agc.process(hop_float, agc_float);

        // 3. Push normalized, gain-adjusted audio to sliding window
        std::memmove(sliding_audio_window.data(), 
                     sliding_audio_window.data() + HOP_STEP, 
                     (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
        std::memcpy(sliding_audio_window.data() + (TF_FRAME_LENGTH - HOP_STEP), 
                    agc_float.data(), 
                    HOP_STEP * sizeof(float));

        // 4. Subtract DC offset (Python Parity)
        float sum = 0.0f;
        for (int i = 0; i < TF_FRAME_LENGTH; ++i) {
            sum += sliding_audio_window[i];
        }
        float mean = sum / static_cast<float>(TF_FRAME_LENGTH);

        for (int i = 0; i < TF_FRAME_LENGTH; ++i) {
            clean_buffer[i] = sliding_audio_window[i] - mean;
        }

        // 5. Extract MFCCs from the clean window
        feature_extractor.compute_mfcc_features(clean_buffer, current_mfccs);

        // 6. Stateful Inference
        std::vector<float> scores = wakeword_model.infer(current_mfccs);
        
        float raw_jarvis_prob = scores.empty() ? 0.0f : scores[0]; 
        float smoothed_jarvis_prob = 0.0f;

        // 7. Process the rolling average envelope
        bool is_hit = jarvis_averager.process(raw_jarvis_prob, smoothed_jarvis_prob);

        if (debug_mode) {
            int bars = static_cast<int>(smoothed_jarvis_prob * 20.0f);
            if (bars > 20) bars = 20;
            std::string vu(bars, '#');
            std::string empty(20 - bars, '-');
            
            // Format gain for display (e.g., "15.0x", " 2.3x")
            char gain_str[10];
            snprintf(gain_str, sizeof(gain_str), "%4.1fx", agc.get_current_gain());

            std::cout << "\r[Gain: " << gain_str << "] [Env: " << vu << empty << "] | Raw: " 
                      << raw_jarvis_prob << " | Avg: " << smoothed_jarvis_prob << "      " << std::flush;
        }

        if (is_hit) {
            if (debug_mode) std::cout << "\n"; 
            std::cout << "\n🔔 WAKE WORD DETECTED! (Smoothed Envelope: " << smoothed_jarvis_prob << ")\n";
            
            // WIPE ALL MEMORY to prevent trailing ghost triggers
            wakeword_model.reset_states();
            jarvis_averager.reset();

            if (debug_mode) std::cout << "   (Memory wiped. Cooldown initiated for 800ms...)\n\n"; 
        }
    }

    if (mic) snd_pcm_close(mic);
    if (wav_file.is_open()) wav_file.close();
    std::cout << "\n[OK] Audio pipeline successfully closed.\n";
    return 0;
}
