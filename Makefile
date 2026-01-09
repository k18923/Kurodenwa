.PHONY: all set-target build clean fullclean flash monitor menuconfig log log-clear log-view log-clean

BUILD_DIR ?= build
TARGET    ?= esp32
PORT      ?= /dev/cu.usbserial-210
BAUD      ?= 115200
IDF_PATH  ?= $(HOME)/.espressif/v5.5.2/esp-idf
IDF_PY    ?= idf.py

all: build

activate:
	@echo "IDF_PATH: $(IDF_PATH)"
	@echo "Activating ESP-IDF environment..."
	$(IDF_PATH)/tools/activate.py

export:
	@echo "Sourcing ESP-IDF export.sh..."
	. $(IDF_PATH)/export.sh 

set-target:
	@if [ ! -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		echo ">> $(IDF_PY) -B $(BUILD_DIR) set-target $(TARGET)"; \
		$(IDF_PY) -B $(BUILD_DIR) set-target $(TARGET); \
	fi

build: set-target
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) build"
	$(IDF_PY) -B $(BUILD_DIR) build

clean:
	@echo ">> /bin/rm -rf $(BUILD_DIR)"
	/bin/rm -rf $(BUILD_DIR)

fullclean:
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) fullclean"
	$(IDF_PY) -B $(BUILD_DIR) fullclean

flash: build
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) -p $(PORT) flash"
	$(IDF_PY) -p $(PORT) flash

monitor:
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor"
	$(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor

menuconfig:
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) menuconfig"
	$(IDF_PY) -B $(BUILD_DIR) menuconfig

# ログをファイルに保存しながらモニタ
# 終了: Ctrl+A → K → y
# ログは log.txt に保存される（screen のデフォルト screenlog.0 を終了後にリネーム）
LOG_FILE ?= screenlog.0
log:
	@rm -f $(LOG_FILE)
	@echo "ログを $(LOG_FILE) に保存中..."
	@echo "終了方法: Ctrl+A → K → y"
	@echo ">> screen -L $(PORT) $(BAUD)"
	screen -L $(PORT) $(BAUD) ; [ -f $(LOG_FILE) ] 

log-clear:
	@rm -f $(LOG_FILE)
	@echo "$(LOG_FILE) をクリアしました"

log-view:
	@echo ">> less $(LOG_FILE)"
	less $(LOG_FILE)

log-clean:
	@echo ">> cat $(LOG_FILE) | col -b | less"
	cat $(LOG_FILE) | col -b | less
