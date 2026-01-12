.PHONY: all set-target build clean fullclean flash monitor menuconfig log log-clear log-view log-clean

BUILD_DIR ?= build
TARGET    ?= esp32
PORT      ?= /dev/cu.usbserial-110
# PORT      ?= /dev/cu.usbserial-210
# PORT      ?= /dev/cu.usbserial-2120
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
	$(IDF_PY) -p $(PORT) -b $(BAUD) flash

monitor:
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor"
	$(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor

menuconfig:
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) menuconfig"
	$(IDF_PY) -B $(BUILD_DIR) menuconfig

# ログをファイルに保存しながらモニタ（idf.py monitor のログ機能）
# ログ開始/終了: Ctrl+T → L
# 終了: Ctrl+] （モニタの終了キー）
LOG_DIR ?= log
log:
	@mkdir -p $(LOG_DIR)
	@echo "ログは自動で開始します（Ctrl+T → L で停止）"
	@echo "終了方法: Ctrl+]"
	@echo "ログファイル: $(LOG_DIR)/log.*.txt"
	@echo ">> $(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor"
	tools/monitor_log.exp $(IDF_PY) -B $(BUILD_DIR) -p $(PORT) monitor

log-clear:
	@rm -f $(LOG_DIR)/log.*.txt
	@echo "$(LOG_DIR)/log.*.txt をクリアしました"

log-view:
	@file=$$(ls -t $(LOG_DIR)/log.*.txt 2>/dev/null | head -1); \
	if [ -z "$$file" ]; then \
		echo "$(LOG_DIR)/log.*.txt がありません"; \
	else \
		echo ">> less -R $$file"; \
		less -R "$$file"; \
	fi

log-clean:
	@file=$$(ls -t $(LOG_DIR)/log.*.txt 2>/dev/null | head -1); \
	if [ -z "$$file" ]; then \
		echo "$(LOG_DIR)/log.*.txt がありません"; \
	else \
		echo ">> cat $$file | col -b | less"; \
		cat "$$file" | col -b | less; \
	fi
