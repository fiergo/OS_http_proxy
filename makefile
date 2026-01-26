CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -lpthread

TARGET = httpproxy
SRCS = httpproxy.c
OBJS = $(SRCS:.c=.o)
BIN_DIR = bin

TARGET_PATH = $(BIN_DIR)/$(TARGET)

.PHONY: all
all: directories $(TARGET_PATH)

directories:
	@mkdir -p $(BIN_DIR)

$(TARGET_PATH): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ HTTP-proxy собран: $(TARGET_PATH)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: run
run: all
	@echo "Запуск HTTP-proxy на порту 8080..."
	@echo "Используйте Ctrl+C для остановки"
	@echo ""
	./$(TARGET_PATH) 8080

.PHONY: run-port
run-port: all
ifndef PORT
	$(error Укажите PORT: make run-port PORT=<номер>)
endif
	@echo "Запуск HTTP-proxy на порту $(PORT)..."
	@echo "Используйте Ctrl+C для остановки"
	@echo ""
	./$(TARGET_PATH) $(PORT)

.PHONY: run-root
run-root: all
	@echo "Запуск HTTP-proxy на порту 80 (требуются права root)..."
	@echo "Используйте Ctrl+C для остановки"
	@echo ""
	sudo ./$(TARGET_PATH) 80

.PHONY: test
test: all
	@echo "Запуск тестов..."
	@chmod +x run_tests.sh
	./run_tests.sh

.PHONY: debug
debug: CFLAGS += -DDEBUG -g -O0
debug: clean all

.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(BIN_DIR)
	@echo "✓ Очистка завершена"

.PHONY: help
help:
	@echo "Доступные команды:"
	@echo "  make all          - Сборка прокси"
	@echo "  make clean        - Очистка"
	@echo "  make run          - Запуск на порту 8080"
	@echo "  make run-port PORT=N - Запуск на порту N"
	@echo "  make run-root     - Запуск на порту 80 (sudo)"
	@echo "  make test         - Запуск всех тестов"
	@echo "  make debug        - Отладочная сборка"
	@echo "  make help         - Эта справка"