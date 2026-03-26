#include "tflite_runner.h"
#include <iostream>

TFLiteRunner::TFLiteRunner(const std::string& model_path) {
    model_ = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!model_) {
        std::cerr << "[!] FATAL: Failed to mmap model: " << model_path << "\n";
        exit(1);
    }
    
    tflite::InterpreterBuilder(*model_, resolver_)(&interpreter_);
    if (!interpreter_) {
        std::cerr << "[!] FATAL: Failed to construct interpreter.\n";
        exit(1);
    }
    
    if (interpreter_->AllocateTensors() != kTfLiteOk) {
        std::cerr << "[!] FATAL: Failed to allocate tensors.\n";
        exit(1);
    }
}

TFLiteRunner::~TFLiteRunner() {}

std::vector<float> TFLiteRunner::infer(const std::vector<float>& input_features) {
    // 1. Copy features to input tensor
    float* input_tensor = interpreter_->typed_input_tensor<float>(0);
    for (size_t i = 0; i < input_features.size(); ++i) {
        input_tensor[i] = input_features[i];
    }
    
    // 2. Run the model
    interpreter_->Invoke();
    
    // 3. Extract the array of output probabilities
    float* output_tensor = interpreter_->typed_output_tensor<float>(0);
    int num_classes = interpreter_->output_tensor(0)->bytes / sizeof(float);
    
    std::vector<float> results(output_tensor, output_tensor + num_classes);
    return results;
}
