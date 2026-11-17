# Makefile for retinaface camera realtime app (Orange Pi A733 NPU)

.DEFAULT_GOAL := all

TARGET     := face_recog_app
BUILD_DIR  := build
SRC_DIR    := src
AI_SDK     := /home/orangepi/ai-sdk

# ---- Sources (all under src/) ----
# Shared across every binary: preprocessing, detectors, tracker, and the
# application logger (spec system-logging). logger.cpp is header-light
# (pimpl) so every target links it via COMMON_SRCS.
COMMON_SRCS := $(SRC_DIR)/detect_pre.cpp \
               $(SRC_DIR)/scrfd_post.cpp \
               $(SRC_DIR)/yolo_post.cpp \
               $(SRC_DIR)/linear_assignment.cpp \
               $(SRC_DIR)/kalman_box_tracker.cpp \
               $(SRC_DIR)/byte_tracker.cpp \
               $(SRC_DIR)/face_track_assoc.cpp \
               $(SRC_DIR)/person_tracker.cpp \
               $(SRC_DIR)/tracker_greedy.cpp \
               $(SRC_DIR)/tracker.cpp \
               $(SRC_DIR)/log/logger.cpp
RECOG_SRCS  := $(SRC_DIR)/face_align.cpp \
               $(SRC_DIR)/face_recog.cpp \
               $(SRC_DIR)/face_db.cpp

# Resident data layer (spec resident-db-layer): SQLite wrapper +
# multi-embedding matcher. Linked into the realtime app. Requires libsqlite3-dev.
DB_SRCS     := $(SRC_DIR)/resident_db.cpp \
               $(SRC_DIR)/resident_catalog.cpp \
               $(SRC_DIR)/match_engine.cpp \
               $(SRC_DIR)/interaction.cpp \
               $(SRC_DIR)/cabin_config.cpp \
               $(SRC_DIR)/edge_client.cpp \
               $(SRC_DIR)/edge_json.cpp \
               $(SRC_DIR)/floor_call_coordinator.cpp \
               $(SRC_DIR)/socketio_client.cpp \
               $(SRC_DIR)/elevator_registers.cpp \
               $(SRC_DIR)/elevator_state.cpp \
               $(SRC_DIR)/call_tracker.cpp

# App-only modules extracted from main.cpp (spec main-cpp-refactor). Used ONLY
# by face_recog_app — do NOT add to COMMON_SRCS (other tools don't need them).
APP_ONLY_SRCS := $(SRC_DIR)/app_config.cpp \
                 $(SRC_DIR)/door_inference.cpp \
                 $(SRC_DIR)/video_io.cpp \
                 $(SRC_DIR)/event_video_recorder.cpp \
                 $(SRC_DIR)/overlay.cpp \
                 $(SRC_DIR)/benchmark.cpp \
                 $(SRC_DIR)/resident_reconcile.cpp \
                 $(SRC_DIR)/system_metrics.cpp \
                 $(SRC_DIR)/occupancy_config.cpp \
                 $(SRC_DIR)/occupancy_state.cpp \
                 $(SRC_DIR)/occupancy_counter.cpp \
                 $(SRC_DIR)/trip_assembler.cpp \
                 $(SRC_DIR)/traffic_db.cpp \
                 $(SRC_DIR)/visitor_gallery.cpp \
                 $(SRC_DIR)/traffic_log_writer.cpp

APP_SRCS_CPP     := $(SRC_DIR)/main.cpp           $(COMMON_SRCS) $(RECOG_SRCS) $(DB_SRCS) $(APP_ONLY_SRCS)
ENROLL_SRCS_CPP  := $(SRC_DIR)/enroll_faces.cpp   $(COMMON_SRCS) \
                    $(SRC_DIR)/face_align.cpp $(SRC_DIR)/face_recog.cpp \
                    $(SRC_DIR)/resident_db.cpp
CAPTURE_SRCS_CPP := $(SRC_DIR)/capture_person.cpp $(COMMON_SRCS)
ADD_SRCS_CPP     := $(SRC_DIR)/add_person.cpp     $(COMMON_SRCS) \
                    $(SRC_DIR)/face_align.cpp $(SRC_DIR)/face_recog.cpp \
                    $(SRC_DIR)/resident_db.cpp $(SRC_DIR)/resident_enroll.cpp

TRAFFIC_ADMIN_SRCS_CPP := $(SRC_DIR)/traffic_admin.cpp \
                          $(SRC_DIR)/traffic_db.cpp \
                          $(SRC_DIR)/trip_assembler.cpp \
                          $(SRC_DIR)/elevator_state.cpp \
                          $(SRC_DIR)/elevator_registers.cpp \
                          $(SRC_DIR)/call_tracker.cpp \
                          $(SRC_DIR)/floor_call_coordinator.cpp \
                          $(SRC_DIR)/edge_json.cpp \
                          $(SRC_DIR)/resident_db.cpp \
                          $(SRC_DIR)/match_engine.cpp \
                          $(SRC_DIR)/log/logger.cpp

SDK_SRCS_C := $(AI_SDK)/examples/libawnn_viplite/awnn_lib.c \
              $(AI_SDK)/examples/libawnn_viplite/awnn_quantize.c

# ---- Includes ----
# -I$(SRC_DIR) so `#include "log/log.h"` and `#include "anchors.h"` resolve
INCLUDES := -I$(SRC_DIR) \
            -I$(AI_SDK) \
            -I$(AI_SDK)/examples/libawnn_viplite \
            -I/usr/include \
            $(shell pkg-config --cflags opencv4)

# ---- Libs ----
# System-installed VIPLite (v2.0.3.2) at /lib/libVIPhal.so, /lib/libNBGlinker.so
LIBS := $(shell pkg-config --libs opencv4) \
        -lVIPhal -lNBGlinker \
        -lsqlite3 \
        -lpthread -lrt -lm -ldl

# ---- Flags ----
CFLAGS   := -O2 -Wall -Wno-unused-function -Wno-unused-variable -g
CXXFLAGS := $(CFLAGS) -std=c++17

# ---- Objects ----
# src/foo.cpp -> build/foo.o (strip src/ prefix)
APP_OBJS     := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(APP_SRCS_CPP))
ENROLL_OBJS  := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(ENROLL_SRCS_CPP))
CAPTURE_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(CAPTURE_SRCS_CPP))
ADD_OBJS     := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(ADD_SRCS_CPP))
TRAFFIC_ADMIN_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(TRAFFIC_ADMIN_SRCS_CPP))
SDK_OBJS     := $(patsubst $(AI_SDK)/%.c,$(BUILD_DIR)/sdk_%.o,$(SDK_SRCS_C))

# Header dependency tracking. Without .d files, changing a shared struct such
# as EmbeddingRow can leave old and new object ABIs linked into one binary.
ALL_OBJS := $(sort $(APP_OBJS) $(ENROLL_OBJS) $(CAPTURE_OBJS) $(ADD_OBJS) $(TRAFFIC_ADMIN_OBJS) $(SDK_OBJS))
DEPS     := $(ALL_OBJS:.o=.d)
-include $(DEPS)

# ---- Rules ----
.PHONY: all clean run format format-check lint

all: $(TARGET) enroll_faces capture_person add_person probe_models traffic_admin

$(TARGET): $(APP_OBJS) $(SDK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "==> Built $@"

enroll_faces: $(ENROLL_OBJS) $(SDK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lstdc++fs
	@echo "==> Built $@"

capture_person: $(CAPTURE_OBJS) $(SDK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lstdc++fs
	@echo "==> Built $@"

add_person: $(ADD_OBJS) $(SDK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lstdc++fs
	@echo "==> Built $@"

probe_models: $(BUILD_DIR)/probe_models.o $(SDK_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "==> Built $@"

traffic_admin: $(TRAFFIC_ADMIN_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lsqlite3 -lpthread -lstdc++fs
	@echo "==> Built $@"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/sdk_%.o: $(AI_SDK)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ---- Developer tooling (never part of `all`) ----
# src/log/log.h is the NPU SDK shim and must not be reformatted.
FORMAT_FILES := $(filter-out $(SRC_DIR)/log/log.h,$(shell git ls-files '*.cpp' '*.h'))

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

lint:
	clang-tidy $(APP_SRCS_CPP) -- $(CXXFLAGS) $(INCLUDES)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) enroll_faces capture_person add_person probe_models traffic_admin

run: $(TARGET)
	./$(TARGET) model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb
