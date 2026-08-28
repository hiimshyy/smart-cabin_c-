#include "face_recog.h"

#include <awnn_lib.h>
#include <opencv2/imgproc.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>

FaceRecognizer::FaceRecognizer(const std::string& model_path,
                               int embedding_dim,
                               bool rgb)
    : path_(model_path), dim_(embedding_dim), rgb_(rgb) {
    ctx_ = awnn_create(model_path.c_str());
    if (!ctx_) {
        fprintf(stderr, "[recog] awnn_create failed for %s\n", model_path.c_str());
    }
    input_buf_.assign(112 * 112 * 3, 0);
}

FaceRecognizer::~FaceRecognizer() {
    if (ctx_) {
        awnn_destroy(ctx_);
        ctx_ = nullptr;
    }
}

bool FaceRecognizer::extract(const cv::Mat& face, std::vector<float>& emb_out) {
    if (!ctx_) {
        static int _e = 0;
        if (++_e <= 3) fprintf(stderr, "[recog] extract: ctx_ is NULL\n");
        return false;
    }
    if (face.empty() || face.type() != CV_8UC3 ||
        face.cols != 112 || face.rows != 112) {
        static int _e = 0;
        if (++_e <= 3) {
            fprintf(stderr, "[recog] bad input: empty=%d type=%d dims=%dx%d "
                    "(expected CV_8UC3 (type=16) 112x112)\n",
                    face.empty(), face.type(), face.cols, face.rows);
        }
        return false;
    }

    // Prepare input buffer HWC uint8. Handle RGB swap if configured.
    // face is BGR from OpenCV.
    if (rgb_) {
        // Swap channels into input_buf_
        cv::Mat rgb;
        cv::cvtColor(face, rgb, cv::COLOR_BGR2RGB);
        // rgb is now HWC uint8 contiguous, 112*112*3 bytes
        std::memcpy(input_buf_.data(), rgb.data, 112 * 112 * 3);
    } else {
        // Feed BGR directly (rare, only if trained that way)
        if (face.isContinuous()) {
            std::memcpy(input_buf_.data(), face.data, 112 * 112 * 3);
        } else {
            cv::Mat cont = face.clone();
            std::memcpy(input_buf_.data(), cont.data, 112 * 112 * 3);
        }
    }

    void* inputs[] = { input_buf_.data() };
    awnn_set_input_buffers(ctx_, inputs);
    awnn_run(ctx_);
    float** outs = awnn_get_output_buffers(ctx_);
    if (!outs || !outs[0]) {
        static int _e = 0;
        if (++_e <= 3) {
            fprintf(stderr, "[recog] NPU output null (outs=%p outs0=%p)\n",
                    (void*)outs, outs ? (void*)outs[0] : nullptr);
        }
        return false;
    }

    // Copy + L2-normalize
    emb_out.assign(outs[0], outs[0] + dim_);
    double sq = 0.0;
    for (int i = 0; i < dim_; ++i) sq += (double)emb_out[i] * emb_out[i];
    float inv = (sq > 1e-12) ? static_cast<float>(1.0 / std::sqrt(sq)) : 0.0f;
    for (int i = 0; i < dim_; ++i) emb_out[i] *= inv;

    return true;
}
