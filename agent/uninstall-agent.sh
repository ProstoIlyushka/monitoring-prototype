#!/bin/bash
# ============================================================
# Полное удаление monitoring-agent
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
    echo "Please run: sudo ./uninstall-agent.sh"
    exit 1
fi

print_info "Starting Monitoring Agent uninstallation..."

# ============================================================
# Подтверждение
# ============================================================
echo ""
print_warning "This will REMOVE the following:"
echo "  📁 Binary:      /usr/local/bin/monitoring-agent"
echo "  📁 Config:      /etc/monitoring-agent/"
echo "  📁 Data:        /var/lib/monitoring-agent/"
echo "  📁 Logs:        /var/log/monitoring-agent/"
echo "  📁 Service:     /etc/systemd/system/monitoring-agent.service"
echo ""
read -p "Are you sure you want to continue? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    print_info "Uninstallation cancelled."
    exit 0
fi

# ============================================================
# Остановка и отключение сервиса
# ============================================================
print_info "Stopping and disabling service..."

if systemctl is-active --quiet monitoring-agent; then
    systemctl stop monitoring-agent
    print_success "Service stopped."
else
    print_info "Service was not running."
fi

if systemctl is-enabled --quiet monitoring-agent 2>/dev/null; then
    systemctl disable monitoring-agent
    print_success "Service disabled."
else
    print_info "Service was not enabled."
fi

# ============================================================
# Удаление systemd сервиса
# ============================================================
SERVICE_FILE="/etc/systemd/system/monitoring-agent.service"
if [ -f "$SERVICE_FILE" ]; then
    rm -f "$SERVICE_FILE"
    print_success "Removed service file: $SERVICE_FILE"
else
    print_info "Service file not found."
fi

systemctl daemon-reload
print_success "Systemd reloaded."

# ============================================================
# Удаление бинарника
# ============================================================
BIN_FILE="/usr/local/bin/monitoring-agent"
if [ -f "$BIN_FILE" ]; then
    rm -f "$BIN_FILE"
    print_success "Removed binary: $BIN_FILE"
else
    print_info "Binary not found."
fi

# ============================================================
# Удаление директорий (с подтверждением для данных)
# ============================================================
CONFIG_DIR="/etc/monitoring-agent"
DATA_DIR="/var/lib/monitoring-agent"
LOG_DIR="/var/log/monitoring-agent"

# Проверяем, есть ли данные в буфере
if [ -d "$DATA_DIR" ] && [ -f "$DATA_DIR/agent_buffer.dat" ]; then
    BUFFER_SIZE=$(stat -c%s "$DATA_DIR/agent_buffer.dat" 2>/dev/null || echo "0")
    if [ "$BUFFER_SIZE" -gt 0 ]; then
        print_warning "Buffer file contains data ($BUFFER_SIZE bytes)."
        read -p "Delete buffer data? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_info "Keeping buffer file at: $DATA_DIR/agent_buffer.dat"
        else
            rm -rf "$DATA_DIR"
            print_success "Removed data directory: $DATA_DIR"
        fi
    else
        rm -rf "$DATA_DIR"
        print_success "Removed data directory: $DATA_DIR"
    fi
elif [ -d "$DATA_DIR" ]; then
    rm -rf "$DATA_DIR"
    print_success "Removed data directory: $DATA_DIR"
fi

# Удаляем конфиг и логи
if [ -d "$CONFIG_DIR" ]; then
    rm -rf "$CONFIG_DIR"
    print_success "Removed config directory: $CONFIG_DIR"
fi

if [ -d "$LOG_DIR" ]; then
    rm -rf "$LOG_DIR"
    print_success "Removed logs directory: $LOG_DIR"
fi

# ============================================================
# Итоговая информация
# ============================================================
echo ""
echo "========================================"
print_success "Uninstallation completed!"
echo "========================================"
echo ""
echo "✅ Monitoring Agent has been removed from the system."
echo ""
print_warning "To reinstall, run: sudo ./install-agent.sh"
echo "========================================"