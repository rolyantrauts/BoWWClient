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
              << "  -t <float>     Detection confidence threshold from 0.0 to 1.0 (default: 0.85)\n"
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
// 2. DC-Blocking High-Pass Filter (1st-Order IIR)
// Removes DC offset (0Hz) and attenuates low-frequency rumble (e.g., 50Hz hum)
// ==============================================================================
class HighPassFilter {
private:
    float alpha_;
    float prev_x_;
    float prev_y_;

public:
    HighPassFilter(float cutoff_freq = 50.0f, float sample_rate = 16000.0f) {
        // Calculate the RC filter coefficient
        float dt = 1.0f / sample_rate;
        float rc = 1.0f / (2.0f * M_PI * cutoff_freq);
        alpha_ = rc / (rc + dt);
        
        prev_x_ = 0.0f;
        prev_y_ = 0.0f;
    }

    void process(float* audio, int length) {
        for (int i = 0; i < length; ++i) {
            float x = audio[i];
            // Difference equation for a 1st-order High Pass Filter
            float y = alpha_ * (prev_y_ + x - prev_x_);
            
            audio[i] = y;
            prev_x_ = x;
            prev_y_ = y;
        }
    }
};

// ==============================================================================
// 3. ALSA Hardware Initialization
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
// 4. Main WakeWord Loop 
// ==============================================================================
int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string alsa_dev = "plughw:Loopback,1,0";
    std::string model_file = "models/wakeword.tflite";
    std::string file_input = "";
    float threshold = 0.85f;
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

    std::cout << "\n[OK] BoWWClient Online (HPF + Multi-Class Mode).\n";
    if (file_input.empty()) {
        std::cout << "     ALSA Device: " << alsa_dev << "\n";
    } else {
        std::cout << "     File Input:  " << file_input << " (Simulated Real-Time)\n";
    }
    std::cout << "     Model File:  " << model_file << "\n";
    std::cout << "     Threshold:   " << threshold << "\n";
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
        // Skip the standard 44-byte WAV header
        wav_file.seekg(44, std::ios::beg); 
    }

    // Initialize the 50Hz High-Pass Filter
    HighPassFilter hpf(50.0f, SAMPLE_RATE);
    FeatureExtractor feature_extractor;

    std::vector<int16_t> audio_buf(HOP_STEP);
    std::vector<float> hop_float(HOP_STEP);
    std::vector<float> sliding_audio_window(TF_FRAME_LENGTH, 0.0f); 
    std::vector<float> current_mfccs(TF_MFCC_BINS, 0.0f);

    std::cout << "\n[OK] Processing Audio...\n\n";

    while (g_running) {
        if (!file_input.empty()) {
            wav_file.read(reinterpret_cast<char*>(audio_buf.data()), HOP_STEP * sizeof(int16_t));
            if (wav_file.gcount() == 0 || wav_file.eof()) {
                std::cout << "\n[OK] Reached end of audio file.\n";
                break;
            }
            // Sleep for 20ms to simulate the exact hardware timing of the live microphone
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
            int frames_read = snd_pcm_readi(mic, audio_buf.data(), HOP_STEP);
            if (frames_read < 0) { 
                snd_pcm_recover(mic, frames_read, 1); 
                continue; 
            }
        }

        // 1. Convert INT16 to Float
        for (int i = 0; i < HOP_STEP; ++i) {
            hop_float[i] = static_cast<float>(audio_buf[i]) / 32768.0f;
        }

        // 2. Apply continuous 50Hz DC-Blocking High-Pass Filter
        hpf.process(hop_float.data(), HOP_STEP);

        // 3. Extract VU Peak AFTER filtering out the DC bias
        float raw_peak = 0.0f;
        for (int i = 0; i < HOP_STEP; ++i) {
            if (std::abs(hop_float[i]) > raw_peak) {
                raw_peak = std::abs(hop_float[i]);
            }
        }

        // 4. Push to sliding window
        std::memmove(sliding_audio_window.data(), 
                     sliding_audio_window.data() + HOP_STEP, 
                     (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
        std::memcpy(sliding_audio_window.data() + (TF_FRAME_LENGTH - HOP_STEP), 
                    hop_float.data(), 
                    HOP_STEP * sizeof(float));

        // 5. Extract MFCCs from the clean window
        feature_extractor.compute_mfcc_features(sliding_audio_window, current_mfccs);

        // 6. Stateful Inference
        std::vector<float> scores = wakeword_model.infer(current_mfccs);
        
        int best_idx = 0;
        float best_score = scores[0];
        for (size_t i = 1; i < scores.size(); ++i) {
            if (scores[i] > best_score) {
                best_score = scores[i];
                best_idx = i;
            }
        }

        std::string best_label = (best_idx < class_labels.size()) ? class_labels[best_idx] : "IDX_" + std::to_string(best_idx);

        if (debug_mode) {
            int bars = static_cast<int>(raw_peak * 20.0f);
            if (bars > 20) bars = 20;
            std::string vu(bars, '#');
            std::string empty(20 - bars, '-');
            
            std::cout << "\r[MIC VU: " << vu << empty << "] | CLASS: " << best_label << " (" << best_score << ")      " << std::flush;
        }

        // Trigger on ANY threshold pass (no cooldown, no wipes)
        if (best_idx == 0 && best_score >= threshold) {
            if (debug_mode) std::cout << "\n"; 
            std::cout << "[!!!] HIT! Class: " << best_label << " | Score: " << best_score << "\n";
        }
    }

    if (mic) snd_pcm_close(mic);
    if (wav_file.is_open()) wav_file.close();
    std::cout << "\n[OK] Audio pipeline successfully closed.\n";
    return 0;
}
