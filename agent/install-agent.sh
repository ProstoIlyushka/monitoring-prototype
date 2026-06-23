#!/bin/bash
# ============================================================
# Установка monitoring-agent из исходников
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
    echo "Please run: sudo ./install-agent.sh"
    exit 1
fi

print_info "Starting Monitoring Agent installation..."

# ============================================================
# Установка зависимостей
# ============================================================
print_info "Installing dependencies..."

apt-get update -qq
apt-get install -y -qq \
    build-essential \
    g++ \
    libcurl4-openssl-dev \
    libjsoncpp-dev \
    jq \
    curl

print_success "Dependencies installed."

# ============================================================
# Определение директорий
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/monitoring-agent"
DATA_DIR="/var/lib/monitoring-agent"
LOG_DIR="/var/log/monitoring-agent"
BUFFER_FILE="$DATA_DIR/agent_buffer.dat"

# ============================================================
# Компиляция агента
# ============================================================
print_info "Compiling agent from source..."

cd "$SCRIPT_DIR"

if [ ! -f "$SCRIPT_DIR/agent.cpp" ]; then
    print_error "agent.cpp not found in $SCRIPT_DIR"
    exit 1
fi

g++ -o monitoring-agent agent.cpp \
    -lcurl \
    -std=c++17 \
    -O2 \
    -static-libstdc++

if [ $? -ne 0 ]; then
    print_error "Compilation failed."
    exit 1
fi

print_success "Compilation completed."

# ============================================================
# Создание директорий
# ============================================================
print_info "Creating directories..."

mkdir -p "$CONFIG_DIR"
mkdir -p "$DATA_DIR"
mkdir -p "$LOG_DIR"

chmod 755 "$DATA_DIR"
chmod 755 "$LOG_DIR"

if [ ! -f "$BUFFER_FILE" ]; then
    touch "$BUFFER_FILE"
    chmod 644 "$BUFFER_FILE"
    print_info "Created buffer file: $BUFFER_FILE"
fi

print_success "Directories created."

# ============================================================
# Копирование бинарника
# ============================================================
print_info "Installing binary..."

cp monitoring-agent "$BIN_DIR/"
chmod 755 "$BIN_DIR/monitoring-agent"

print_success "Binary installed to $BIN_DIR/monitoring-agent"

# ============================================================
# Создание конфигурации
# ============================================================
print_info "Creating configuration..."

CONFIG_FILE="$CONFIG_DIR/config"

if [ -f "$CONFIG_FILE" ]; then
    print_warning "Config file already exists: $CONFIG_FILE"
    read -p "Do you want to overwrite it? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_info "Keeping existing config file."
    else
        rm "$CONFIG_FILE"
    fi
fi

if [ ! -f "$CONFIG_FILE" ]; then
    cat > "$CONFIG_FILE" << 'EOF'
# Monitoring Agent Configuration
# ============================================================
# API_KEY - your agent authentication key
# Get it from: curl -k https://your-server:8443/api/agents
# ============================================================
API_KEY=CHANGE_ME

# SERVER_URL - central server endpoint for metrics
# Default: https://localhost:8443/api/metrics
# ============================================================
SERVER_URL=https://localhost:8443/api/metrics
EOF
    chmod 600 "$CONFIG_FILE"
    print_success "Config file created: $CONFIG_FILE"
    print_warning "Please edit $CONFIG_FILE and set API_KEY."
    echo "  sudo nano $CONFIG_FILE"
fi

# ============================================================
# Настройка параметров (если переданы аргументами)
# ============================================================
if [ -n "$1" ]; then
    print_info "Setting API_KEY from command line..."
    sed -i "s/API_KEY=.*/API_KEY=$1/" "$CONFIG_FILE"
    print_success "API_KEY set to: $1"
fi

if [ -n "$2" ]; then
    print_info "Setting SERVER_URL from command line..."
    sed -i "s|SERVER_URL=.*|SERVER_URL=$2|" "$CONFIG_FILE"
    print_success "SERVER_URL set to: $2"
fi

# ============================================================
# Создание systemd сервиса
# ============================================================
print_info "Creating systemd service..."

SERVICE_FILE="/etc/systemd/system/monitoring-agent.service"

cat > "$SERVICE_FILE" << 'EOF'
[Unit]
Description=Monitoring Agent for Linux Servers
After=network.target
Wants=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/var/lib/monitoring-agent
ExecStart=/usr/local/bin/monitoring-agent
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal
MemoryMax=100M
Nice=-5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload

print_success "Systemd service created: $SERVICE_FILE"

# ============================================================
# Запуск сервиса (по желанию)
# ============================================================
echo ""
read -p "Do you want to start the agent now? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    print_info "Starting monitoring agent..."
    
    if grep -q "API_KEY=CHANGE_ME" "$CONFIG_FILE"; then
        print_error "Please set API_KEY in $CONFIG_FILE first!"
        echo "  sudo nano $CONFIG_FILE"
        echo "Then run: sudo systemctl start monitoring-agent"
    else
        systemctl enable monitoring-agent
        systemctl start monitoring-agent
        print_success "Agent started."
        echo ""
        echo "To check status:"
        echo "  sudo systemctl status monitoring-agent"
        echo ""
        echo "To view logs:"
        echo "  sudo journalctl -u monitoring-agent -f"
    fi
else
    print_info "To start the agent manually:"
    echo "  sudo systemctl enable monitoring-agent"
    echo "  sudo systemctl start monitoring-agent"
fi

# ============================================================
# Итоговая информация
# ============================================================
echo ""
echo "========================================"
print_success "Installation completed!"
echo "========================================"
echo ""
echo "📁 Binary:      $BIN_DIR/monitoring-agent"
echo "📁 Config:      $CONFIG_DIR/config"
echo "📁 Data:        $DATA_DIR"
echo "📁 Buffer file: $BUFFER_FILE"
echo "📁 Logs:        $LOG_DIR"
echo "📁 Service:     $SERVICE_FILE"
echo ""
print_warning "Don't forget to set API_KEY in $CONFIG_DIR/config"
echo "  sudo nano $CONFIG_DIR/config"
echo ""
echo "Required settings in config:"
echo "  API_KEY=your-key-here"
echo "  SERVER_URL=https://your-server:8443/api/metrics"
echo ""

if [ -z "$1" ]; then
    echo "Or run with parameters:"
    echo "  sudo ./install-agent.sh <API_KEY> [SERVER_URL]"
    echo ""
    echo "Examples:"
    echo "  sudo ./install-agent.sh test-key-123"
    echo "  sudo ./install-agent.sh test-key-123 https://monitoring.example.com:8443/api/metrics"
fi

echo "========================================"