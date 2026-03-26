#ifndef TFLITE_RUNNER_H
#define TFLITE_RUNNER_H

#include <string>
#include <vector>
#include <memory>
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

class TFLiteRunner {
public:
    TFLiteRunner(const std::string& model_path);
    ~TFLiteRunner();

    // Now returns a vector of probabilities for all output classes
    std::vector<float> infer(const std::vector<float>& input_features);

private:
    std::unique_ptr<tflite::FlatBufferModel> model_;
    tflite::ops::builtin::BuiltinOpResolver resolver_;
    std::unique_ptr<tflite::Interpreter> interpreter_;
};

#endif // TFLITE_RUNNER_H
