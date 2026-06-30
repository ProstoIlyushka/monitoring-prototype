#!/bin/bash
# ============================================================
# Установка сервера мониторинга
# ============================================================
# 
# Скрипт выполняет:
#   1. Генерацию SSL-сертификатов
#   2. Настройку SMTP-конфига
#   3. Сборку Docker-контейнеров
# ============================================================

set -e

# ============================================================
# Цвета для вывода
# ============================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}ℹ️  $1${NC}"; }
print_success() { echo -e "${GREEN}✅ $1${NC}"; }
print_error() { echo -e "${RED}❌ $1${NC}"; }
print_warning() { echo -e "${YELLOW}⚠️  $1${NC}"; }

# ============================================================
# Проверка прав
# ============================================================
if [ "$EUID" -ne 0 ]; then
    print_error "This script must be run as root (sudo)."
    echo "Please run: sudo ./setup.sh"
    exit 1
fi

print_info "🚀 Настройка сервера мониторинга..."

# ============================================================
# Определение директорий
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

print_info "Project root: $PROJECT_ROOT"

# ============================================================
# Проверка Docker
# ============================================================
if ! command -v docker &> /dev/null; then
    print_error "Docker не установлен"
    exit 1
fi
print_success "Docker найден: $(docker --version | head -1)"

if ! docker compose version &> /dev/null; then
    print_error "Docker Compose не найден"
    exit 1
fi
print_success "Docker Compose найден"

# ============================================================
# 1. Генерация SSL-сертификатов
# ============================================================
print_info "1. Генерация SSL-сертификатов..."

SSL_DIR="$PROJECT_ROOT/ssl"
mkdir -p "$SSL_DIR"

if [ -f "$SSL_DIR/cert.pem" ] && [ -f "$SSL_DIR/key.pem" ]; then
    print_warning "SSL-сертификаты уже существуют"
    read -p "Перегенерировать? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_info "Сертификаты сохранены"
    else
        rm -f "$SSL_DIR/cert.pem" "$SSL_DIR/key.pem"
    fi
fi

if [ ! -f "$SSL_DIR/cert.pem" ] || [ ! -f "$SSL_DIR/key.pem" ]; then
    if ! command -v openssl &> /dev/null; then
        print_info "Установка OpenSSL..."
        apt-get update -qq
        apt-get install -y -qq openssl
    fi
    
    openssl req -x509 -newkey rsa:4096 \
        -keyout "$SSL_DIR/key.pem" \
        -out "$SSL_DIR/cert.pem" \
        -days 365 -nodes \
        -subj "/CN=localhost" \
        -addext "subjectAltName = DNS:localhost,IP:127.0.0.1" \
        2>/dev/null
    
    chmod 600 "$SSL_DIR/key.pem"
    chmod 644 "$SSL_DIR/cert.pem"
    print_success "Сертификаты созданы"
fi

# ============================================================
# 2. Настройка SMTP-конфига
# ============================================================
print_info "2. Настройка SMTP-конфига..."

CONFIG_DIR="$PROJECT_ROOT/config"
mkdir -p "$CONFIG_DIR"

CONFIG_FILE="$CONFIG_DIR/server.conf"

if [ -f "$CONFIG_FILE" ]; then
    print_warning "SMTP-конфиг уже существует: $CONFIG_FILE"
    read -p "Пересоздать? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_info "Конфиг сохранён"
    else
        rm -f "$CONFIG_FILE"
    fi
fi

if [ ! -f "$CONFIG_FILE" ]; then
    cat > "$CONFIG_FILE" << 'EOF'
# SMTP Settings for email alerts
SMTP_HOST=smtp.yandex.com
SMTP_PORT=465
SMTP_FROM=your-email@yandex.com
SMTP_PASSWORD=your-app-password
SMTP_TO=admin@example.com
EOF
    chmod 600 "$CONFIG_FILE"
    print_success "SMTP-конфиг создан: $CONFIG_FILE"
    print_warning "Отредактируйте $CONFIG_FILE для настройки email"
    echo "  sudo nano $CONFIG_FILE"
fi

# ============================================================
# 3. Сборка Docker-контейнеров
# ============================================================
print_info "3. Сборка Docker-контейнеров..."

cd "$PROJECT_ROOT"
print_info "Сборка образов (это может занять 2-5 минут)..."

docker compose build --no-cache

print_success "Контейнеры собраны"

# ============================================================
# Завершение
# ============================================================
echo ""
echo "========================================"
print_success "✅ Настройка сервера завершена!"
echo "========================================"
echo ""
echo "Далее:"
echo "  1. Запустите систему: docker compose up -d"
echo "  2. Проверьте статус: docker compose ps"
echo "  3. Откройте дашборд: https://localhost:8443/dashboard"
echo "     Логин: admin / admin"
echo ""
echo "Для просмотра логов: docker compose logs -f"
echo "Для остановки: docker compose down"
echo "========================================"