#include "tflite_runner.h"
#include <iostream>
#include <cstring>
#include <algorithm>

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
    
    // Suggest using 4 threads to match the Python performance optimization
    interpreter_->SetNumThreads(4); 

    if (interpreter_->AllocateTensors() != kTfLiteOk) {
        std::cerr << "[!] FATAL: Failed to allocate tensors.\n";
        exit(1);
    }

    // Safely zero out all hidden input states on boot
    for (size_t i = 1; i < interpreter_->inputs().size(); ++i) {
        int in_tensor_idx = interpreter_->inputs()[i];
        TfLiteTensor* tensor = interpreter_->tensor(in_tensor_idx);
        
        if (tensor != nullptr && tensor->data.raw != nullptr) {
            std::memset(tensor->data.raw, 0, tensor->bytes);
        }
    }
}

TFLiteRunner::~TFLiteRunner() {}

std::vector<float> TFLiteRunner::infer(const std::vector<float>& input_features) {
    // 1. Copy features safely to the primary input tensor (Index 0)
    int first_input_idx = interpreter_->inputs()[0];
    TfLiteTensor* input_tensor = interpreter_->tensor(first_input_idx);
    
    if (input_tensor && input_tensor->data.raw) {
        std::memcpy(input_tensor->data.raw, input_features.data(), input_features.size() * sizeof(float));
    }
    
    // 2. Invoke the model
    if (interpreter_->Invoke() != kTfLiteOk) {
        std::cerr << "[!] Error invoking TFLite interpreter!\n";
    }
    
    // 3. Extract the array of output probabilities (Index 0)
    int first_output_idx = interpreter_->outputs()[0];
    TfLiteTensor* output_tensor = interpreter_->tensor(first_output_idx);
    int num_classes = output_tensor->bytes / sizeof(float);
    
    float* out_ptr = reinterpret_cast<float*>(output_tensor->data.raw);
    std::vector<float> results(out_ptr, out_ptr + num_classes);

    // 4. Safely copy the new memory states back to inputs for the next 20ms frame
    size_t num_states = std::min(interpreter_->inputs().size(), interpreter_->outputs().size()) - 1;
    
    for (size_t i = 1; i <= num_states; ++i) {
        TfLiteTensor* out_state = interpreter_->tensor(interpreter_->outputs()[i]);
        TfLiteTensor* in_state  = interpreter_->tensor(interpreter_->inputs()[i]);

        if (out_state && in_state && out_state->data.raw && in_state->data.raw) {
            size_t copy_bytes = std::min(out_state->bytes, in_state->bytes);
            std::memcpy(in_state->data.raw, out_state->data.raw, copy_bytes);
        }
    }
    
    return results;
}

void TFLiteRunner::reset_states() {
    // Zero out all hidden input states (Tensors 1..N) to wipe the model's memory
    size_t num_states = std::min(interpreter_->inputs().size(), interpreter_->outputs().size()) - 1;
    
    for (size_t i = 1; i <= num_states; ++i) {
        TfLiteTensor* in_state = interpreter_->tensor(interpreter_->inputs()[i]);
        if (in_state && in_state->data.raw) {
            std::memset(in_state->data.raw, 0, in_state->bytes);
        }
    }
}
