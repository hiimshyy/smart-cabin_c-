# Makefile for retinaface camera realtime app (Orange Pi A733 NPU)

TARGET     := face_recog_app
BUILD_DIR  := build
SRC_DIR    := src
AI_SDK     := /home/orangepi/ai-sdk

# ---- Sources (all under src/) ----
COMMON_SRCS := $(SRC_DIR)/detect_pre.cpp \
               $(SRC_DIR)/scrfd_post.cpp \
               $(SRC_DIR)/yolo_post.cpp \
               $(SRC_DIR)/tracker.cpp
RECOG_SRCS  := $(SRC_DIR)/face_align.cpp \
               $(SRC_DIR)/face_recog.cpp \
               $(SRC_DIR)/face_db.cpp

# Resident data layer (spec resident-db-layer): SQLite wrapper +
# multi-embedding matcher. Linked into the realtime app; migrate_fdb
# reuses these too. Requires libsqlite3-dev (apt: libsqlite3-dev).
DB_SRCS     := $(SRC_DIR)/resident_db.cpp \
               $(SRC_DIR)/match_engine.cpp \
               $(SRC_DIR)/interaction.cpp

APP_SRCS_CPP     := $(SRC_DIR)/main.cpp           $(COMMON_SRCS) $(RECOG_SRCS) $(DB_SRCS)
ENROLL_SRCS_CPP  := $(SRC_DIR)/enroll_faces.cpp   $(COMMON_SRCS) $(RECOG_SRCS)
CAPTURE_SRCS_CPP := $(SRC_DIR)/capture_person.cpp $(COMMON_SRCS)
ADD_SRCS_CPP     := $(SRC_DIR)/add_person.cpp     $(COMMON_SRCS) $(RECOG_SRCS)

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
SDK_OBJS     := $(patsubst $(AI_SDK)/%.c,$(BUILD_DIR)/sdk_%.o,$(SDK_SRCS_C))

# ---- Rules ----
.PHONY: all clean run

all: $(TARGET) enroll_faces capture_person add_person probe_models

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

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/sdk_%.o: $(AI_SDK)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) enroll_faces capture_person add_person probe_models

run: $(TARGET)
	./$(TARGET) model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb
