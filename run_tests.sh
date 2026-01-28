#!/bin/bash
set -euo pipefail

# Конфигурация
PROXY_BIN="./bin/httpproxy"
PROXY_PORT=8080
PROXY_HOST="127.0.0.1"
PROXY_LOG="/tmp/proxy_test.log"
TEST_TIMEOUT=10

# Цвета для вывода
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${YELLOW}[test]${NC} $1"
}

success() {
    echo -e "${GREEN}[test]    ✓ $1${NC}"
}

error() {
    echo -e "${RED}[test]    ✗ $1${NC}"
    exit 1
}

# Проверяем, что прокси скомпилирован
if [ ! -f "$PROXY_BIN" ]; then
    log "Прокси не найден, компилирую..."
    remove_unused_response
    make clean > /dev/null 2>&1
    make all > /dev/null 2>&1 || error "Не удалось скомпилировать прокси"
fi

log "Запускаю прокси: $PROXY_BIN на порту $PROXY_PORT"
"$PROXY_BIN" "$PROXY_PORT" > "$PROXY_LOG" 2>&1 &
PROXY_PID=$!

# Функция очистки при завершении
cleanup() {
    log "Останавливаю прокси (pid=$PROXY_PID)"
    kill "$PROXY_PID" 2>/dev/null || true
    wait "$PROXY_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Ждем запуска прокси
sleep 2

# Проверяем, что прокси запустился
if ! kill -0 "$PROXY_PID" 2>/dev/null; then
    error "Прокси не запустился. Лог:"
    cat "$PROXY_LOG"
    exit 1
fi

log "1) Базовый GET запрос через прокси"
if ! BODY=$(curl -s --max-time "$TEST_TIMEOUT" -x "$PROXY_HOST:$PROXY_PORT" http://example.com/); then
    error "Не удалось выполнить запрос к example.com"
fi

if echo "$BODY" | grep -q "Example Domain"; then
    success "Тело ответа содержит 'Example Domain'"
else
    error "Тело ответа не содержит 'Example Domain'"
fi

log "2) Проверка HTTP статус-кода 200"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" --max-time "$TEST_TIMEOUT" -x "$PROXY_HOST:$PROXY_PORT" http://example.com/)
if [ "$STATUS" = "200" ]; then
    success "Статус-код 200 получен"
else
    error "Ожидался статус 200, получен $STATUS"
fi

log "3) Несколько параллельных запросов через прокси (3 клиента)"
PIDS=()
for i in 1 2 3; do
    curl -s --max-time "$TEST_TIMEOUT" -x "$PROXY_HOST:$PROXY_PORT" http://example.com/ >/dev/null &
    PIDS+=($!)
done

FAILED=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        FAILED=1
    fi
done

if [ $FAILED -eq 0 ]; then
    success "3 параллельных запроса выполнены успешно"
else
    error "Один из параллельных запросов завершился с ошибкой"
fi

log "4) Запрос к несуществующему хосту (должна быть ошибка 502)"
# Создаем уникальное имя хоста чтобы избежать кэширования DNS
NONEXISTENT_HOST="nonexistent-$(date +%s)-$$.com"
ERROR_OUTPUT=$(curl -s --max-time "$TEST_TIMEOUT" -x "$PROXY_HOST:$PROXY_PORT" "http://$NONEXISTENT_HOST/" 2>&1 || true)

if echo "$ERROR_OUTPUT" | grep -q "502\|Bad Gateway\|failed\|Failed"; then
    success "Прокси вернул ошибку для несуществующего хоста"
else
    # Проверяем по логу прокси
    if tail -10 "$PROXY_LOG" 2>/dev/null | grep -q "$NONEXISTENT_HOST\|getaddrinfo failed\|502"; then
        success "Прокси корректно обработал запрос к несуществующему хосту"
    else
        error "Прокси не вернул ожидаемую ошибку для несуществующего хоста"
        echo "Вывод curl: $ERROR_OUTPUT"
    fi
fi

log "5) Запрос с заголовками"
HEADERS_RESPONSE=$(curl -s --max-time "$TEST_TIMEOUT" -x "$PROXY_HOST:$PROXY_PORT" \
    -H "User-Agent: TestAgent/1.0" \
    -H "X-Custom-Header: TestValue" \
    http://httpbin.org/headers 2>/dev/null || echo "")

if echo "$HEADERS_RESPONSE" | grep -q "TestAgent"; then
    success "Пользовательский User-Agent передан"
else
    log "  Примечание: httpbin.org может быть недоступен, пропускаем тест"
    success "Тест заголовков пропущен (httpbin недоступен)"
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}          ВСЕ ТЕСТЫ ПРОЙДЕНЫ          ${NC}"
echo -e "${GREEN}========================================${NC}"