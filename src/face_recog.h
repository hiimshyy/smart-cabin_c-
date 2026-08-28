#pragma once
// Face recognition wrapper — 2nd NPU context, extract L2-normalized embedding.
// Works with MobileFaceNet / ArcFace R50 style models (input 112x112).

#include <opencv2/core.hpp>
#include <string>
#include <vector>
#include <memory>

struct Awnn_Context;
typedef struct Awnn_Context Awnn_Context_t;

class FaceRecognizer {
public:
    // model_path: path to .nb file (must have 112x112x3 uint8 input,
    //             output: 1-D float embedding of length `embedding_dim`).
    // embedding_dim: 128 for classic MobileFaceNet, 512 for buffalo_l models.
    // rgb: if true, swap BGR→RGB before feeding to NPU (most InsightFace
    //      models were trained with RGB inputs).
    FaceRecognizer(const std::string& model_path,
                   int embedding_dim = 512,
                   bool rgb = true);
    ~FaceRecognizer();

    // Extract embedding from a 112x112 BGR uint8 aligned face image.
    // Output vector is L2-normalized (unit-norm).
    // Returns false on any error (bad size, NPU failure).
    bool extract(const cv::Mat& aligned_bgr_112, std::vector<float>& emb_out);

    int embedding_dim() const { return dim_; }
    const std::string& model_path() const { return path_; }
    bool               model_loaded() const { return ctx_ != nullptr; }

private:
    std::string       path_;
    int               dim_;
    bool              rgb_;
    Awnn_Context_t*   ctx_ = nullptr;
    std::vector<uint8_t> input_buf_;   // 112*112*3
};
