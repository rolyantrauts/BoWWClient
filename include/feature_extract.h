#ifndef FEATURE_EXTRACT_H
#define FEATURE_EXTRACT_H

#include <vector>

struct PFFFT_Setup;

class FeatureExtractor {
public:
    FeatureExtractor();
    ~FeatureExtractor();

    // Takes exactly 640 raw float samples
    // Outputs exactly 20 MFCC features
    void compute_mfcc_features(const std::vector<float>& raw_frame, std::vector<float>& mfcc_out);

private:
    PFFFT_Setup* pffft_setup_;
    
    float* fft_input_;
    float* fft_output_;
    float* fft_work_;
};

#endif // FEATURE_EXTRACT_H
