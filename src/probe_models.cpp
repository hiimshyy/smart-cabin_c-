// probe_models: verify NPU can load YOLO + SCRFD + MobileFaceNet all at once,
// dump each model's tensor layout, AND benchmark inference latency.
//
// Usage:
//   probe_models --scrfd model/face_det/scrfd_..._a733.nb \
//                --recog model/face_recog/w600k_..._a733.nb \
//                --yolo model/person_det/yolov5s_rt_..._a733.nb \
//                [--yolo model/person_det/yolo11s_..._a733.nb ...] \
//                [--iters 20]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

#include <awnn_lib.h>
#define time_begin _probe_time_begin_unused
#define time_end   _probe_time_end_unused
#include <awnn_internal.h>
#undef time_begin
#undef time_end

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct ModelInfo {
    std::string      label;
    std::string      path;
    Awnn_Context_t*  ctx = nullptr;
    unsigned int     input_elements = 0;   // count in the primary input
    std::vector<uint8_t> input_buf;
    double           avg_ms = 0.0;
    double           min_ms = 0.0;
    double           max_ms = 0.0;
};

static void dump_ctx(const ModelInfo& m) {
    Awnn_Context_t* ctx = m.ctx;
    if (!ctx) { printf("  [%s]: NOT LOADED\n", m.label.c_str()); return; }
    printf("  [%s] inputs=%u outputs=%u\n",
           m.label.c_str(), ctx->input_count, ctx->output_count);
    for (unsigned int i = 0; i < ctx->input_count; ++i) {
        const auto& p = ctx->input_params[i];
        printf("    in[%u]  '%s' elements=%u dims=%u [",
               i, p.name, p.elements, p.vip_param.num_of_dims);
        for (unsigned int d = 0; d < p.vip_param.num_of_dims; ++d) {
            printf("%s%u", d ? "x" : "", p.vip_param.sizes[d]);
        }
        printf("]\n");
    }
    for (unsigned int i = 0; i < ctx->output_count; ++i) {
        const auto& p = ctx->output_params[i];
        printf("    out[%u] '%s' elements=%u dims=%u [",
               i, p.name, p.elements, p.vip_param.num_of_dims);
        for (unsigned int d = 0; d < p.vip_param.num_of_dims; ++d) {
            printf("%s%u", d ? "x" : "", p.vip_param.sizes[d]);
        }
        printf("]\n");
    }
}

static bool load_model(ModelInfo& m) {
    printf("\n=== Loading [%s]: %s ===\n", m.label.c_str(), m.path.c_str());
    m.ctx = awnn_create(m.path.c_str());
    if (!m.ctx) {
        fprintf(stderr, "  FAIL to load %s\n", m.path.c_str());
        return false;
    }
    m.input_elements = m.ctx->input_params[0].elements;
    m.input_buf.assign(m.input_elements, 0);   // all-zero uint8 dummy input
    dump_ctx(m);
    return true;
}

static void bench_model(ModelInfo& m, int iters) {
    if (!m.ctx) return;
    void* ins[] = { m.input_buf.data() };
    // Warmup
    awnn_set_input_buffers(m.ctx, ins);
    awnn_run(m.ctx);
    (void)awnn_get_output_buffers(m.ctx);

    double sum = 0.0, mn = 1e9, mx = 0.0;
    for (int i = 0; i < iters; ++i) {
        double t0 = now_ms();
        awnn_set_input_buffers(m.ctx, ins);
        awnn_run(m.ctx);
        (void)awnn_get_output_buffers(m.ctx);
        double dt = now_ms() - t0;
        sum += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    m.avg_ms = sum / iters;
    m.min_ms = mn;
    m.max_ms = mx;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s [--iters N] [--scrfd PATH] [--recog PATH] [--yolo PATH]...\n"
            "  Each --yolo adds one YOLO model to benchmark.\n"
            "  All models must load simultaneously (validates NPU capacity).\n",
            argv[0]);
        return 1;
    }

    int iters = 20;
    std::string scrfd_path, recog_path;
    std::vector<std::string> yolo_paths;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); exit(2); }
            return argv[++i];
        };
        if      (a == "--iters") iters = std::atoi(next());
        else if (a == "--scrfd") scrfd_path = next();
        else if (a == "--recog") recog_path = next();
        else if (a == "--yolo")  yolo_paths.push_back(next());
    }

    printf("=== NPU init (iters=%d per model) ===\n", iters);
    awnn_init();

    // Order matches production: recog first, then detect, then yolo
    std::vector<ModelInfo> models;
    if (!recog_path.empty()) models.push_back({"recog", recog_path});
    if (!scrfd_path.empty()) models.push_back({"scrfd", scrfd_path});
    for (const auto& yp : yolo_paths) {
        std::string label = "yolo:" + yp.substr(yp.find_last_of('/') + 1);
        // Truncate long label
        if (label.size() > 40) label = label.substr(0, 37) + "...";
        models.push_back({label, yp});
    }

    bool all_ok = true;
    for (auto& m : models) if (!load_model(m)) all_ok = false;

    printf("\n=== ALL LOADED ? %s ===\n", all_ok ? "YES" : "NO — NPU cannot hold all");

    if (all_ok) {
        printf("\n=== BENCHMARK (n=%d iters, dummy zero input) ===\n", iters);
        for (auto& m : models) {
            printf("  [%s] running...\n", m.label.c_str()); fflush(stdout);
            bench_model(m, iters);
        }
        printf("\n=== RESULTS ===\n");
        printf("  %-40s  avg ms   min ms   max ms\n", "model");
        printf("  %-40s  ------   ------   ------\n", "-----");
        for (const auto& m : models) {
            printf("  %-40s  %6.2f   %6.2f   %6.2f\n",
                   m.label.c_str(), m.avg_ms, m.min_ms, m.max_ms);
        }
    }

    for (auto& m : models) if (m.ctx) awnn_destroy(m.ctx);
    awnn_uninit();
    return all_ok ? 0 : 2;
}
