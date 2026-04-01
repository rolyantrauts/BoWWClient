#include "feature_extract.h"
#include "pffft.h"
#include "tf_dsp_weights.h"

#include <iostream>
#include <cmath>
#include <cstring>

FeatureExtractor::FeatureExtractor() {
    // Initialize PFFFT for the zero-padded 1024-point transform
    pffft_setup_ = pffft_new_setup(TF_FFT_LENGTH, PFFFT_REAL);
    if (!pffft_setup_) {
        std::cerr << "[!] FATAL: Failed to initialize PFFFT setup.\n";
        exit(1);
    }

    fft_input_  = (float*)pffft_aligned_malloc(TF_FFT_LENGTH * sizeof(float));
    fft_output_ = (float*)pffft_aligned_malloc(TF_FFT_LENGTH * sizeof(float));
    fft_work_   = (float*)pffft_aligned_malloc(TF_FFT_LENGTH * sizeof(float));

    if (!fft_input_ || !fft_output_ || !fft_work_) {
        std::cerr << "[!] FATAL: Failed to allocate PFFFT aligned memory.\n";
        exit(1);
    }
}

FeatureExtractor::~FeatureExtractor() {
    if (fft_input_)  pffft_aligned_free(fft_input_);
    if (fft_output_) pffft_aligned_free(fft_output_);
    if (fft_work_)   pffft_aligned_free(fft_work_);
    if (pffft_setup_) pffft_destroy_setup(pffft_setup_);
}

void FeatureExtractor::compute_mfcc_features(const std::vector<float>& raw_frame, std::vector<float>& mfcc_out) {
    if (raw_frame.size() != TF_FRAME_LENGTH || mfcc_out.size() != TF_MFCC_BINS) {
        std::cerr << "[!] ERROR: FeatureExtractor buffer size mismatch.\n";
        return;
    }

    // 1. Apply Hann Window to the 640 samples
    for (int i = 0; i < TF_FRAME_LENGTH; ++i) {
        fft_input_[i] = raw_frame[i] * tf_hann_window[i];
    }
    
    // 2. Zero-pad the rest up to 1024
    for (int i = TF_FRAME_LENGTH; i < TF_FFT_LENGTH; ++i) {
        fft_input_[i] = 0.0f;
    }

    // 3. Execute FFT
    pffft_transform_ordered(pffft_setup_, fft_input_, fft_output_, fft_work_, PFFFT_FORWARD);

    // 4. Unpack PFFFT format into 513-bin Power Spectrogram
    float mag_sq[TF_SPECTROGRAM_BINS];
    mag_sq[0] = fft_output_[0] * fft_output_[0];                               
    mag_sq[TF_SPECTROGRAM_BINS - 1] = fft_output_[1] * fft_output_[1];         
    
    for (int k = 1; k < TF_SPECTROGRAM_BINS - 1; ++k) {
        float re = fft_output_[k * 2];
        float im = fft_output_[k * 2 + 1];
        mag_sq[k] = (re * re) + (im * im);
    }

    // 5. Matrix Multiply with TF Mel Weights & Apply Log (stabilized)
    float log_mel[TF_MEL_BINS];
    for (int m = 0; m < TF_MEL_BINS; ++m) {
        float dot_product = 0.0f;
        for (int k = 0; k < TF_SPECTROGRAM_BINS; ++k) {
            dot_product += mag_sq[k] * tf_mel_matrix[k][m];
        }
        // TensorFlow uses log (natural log) for MFCCs, not log10
        // log_mel[m] = std::log(dot_product + 1e-6f);
        log_mel[m] = std::log(dot_product + 1e-12f);
    }

    // 6. Apply DCT to get MFCCs
    for (int i = 0; i < TF_MFCC_BINS; ++i) {
        float dct_dot = 0.0f;
        for (int m = 0; m < TF_MEL_BINS; ++m) {
            dct_dot += log_mel[m] * tf_dct_matrix[m][i];
        }
        mfcc_out[i] = dct_dot;
    }
}
