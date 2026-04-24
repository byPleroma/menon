#!/bin/bash
# Script de instalação do OPM (Optimenum)

set -e

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Função para imprimir mensagens
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Verificar se está rodando como root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        print_error "Este script precisa ser executado como root (use sudo)"
        exit 1
    fi
}

# Verificar dependências
check_dependencies() {
    print_info "Verificando dependências..."
    
    local missing_deps=()
    
    # Verificar gcc
    if ! command -v gcc &> /dev/null; then
        missing_deps+=("gcc")
    fi
    
    # Verificar pkg-config
    if ! command -v pkg-config &> /dev/null; then
        missing_deps+=("pkg-config")
    fi
    
    # Verificar GTK3
    if ! pkg-config --exists gtk+-3.0; then
        missing_deps+=("libgtk-3-dev")
    fi
    
    # Verificar GIO
    if ! pkg-config --exists gio-unix-2.0; then
        missing_deps+=("libglib2.0-dev")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        print_error "Dependências faltando: ${missing_deps[*]}"
        echo ""
        print_info "Instale as dependências com:"
        if command -v apt-get &> /dev/null; then
            echo "  sudo apt-get install ${missing_deps[*]}"
        elif command -v pacman &> /dev/null; then
            echo "  sudo pacman -S ${missing_deps[*]}"
        elif command -v dnf &> /dev/null; then
            echo "  sudo dnf install ${missing_deps[*]}"
        else
            print_warning "Não foi possível detectar o gerenciador de pacotes"
        fi
        exit 1
    fi
    
    print_info "Todas as dependências estão instaladas"
}

# Compilar o programa
compile_opm() {
    print_info "Compilando OPM..."
    
    gcc -O3 -march=native -mtune=native -flto -pipe -fomit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -o opm opm-main.c `pkg-config --cflags --libs gtk+-3.0 gio-unix-2.0`
    
    if [ $? -eq 0 ]; then
        print_info "Compilação concluída com sucesso"
    else
        print_error "Erro na compilação"
        exit 1
    fi
}

# Instalar o binário
install_binary() {
    print_info "Instalando binário em /usr/local/bin..."
    
    cp opm /usr/local/bin/
    chmod +x /usr/local/bin/opm
    
    print_info "Binário instalado em /usr/local/bin/opm"
}

# Criar arquivo .desktop
create_desktop_file() {
    print_info "Criando arquivo .desktop..."
    
    mkdir -p /usr/share/applications
    
    cat > /usr/share/applications/opm.desktop <<EOF
[Desktop Entry]
Name=OPM
Comment=Optimenum Application Launcher
Exec=/usr/local/bin/opm
Icon=applications-other
Terminal=false
Type=Application
Categories=Utility;Application;
StartupNotify=true
EOF
    
    chmod 644 /usr/share/applications/opm.desktop
    
    print_info "Arquivo .desktop criado em /usr/share/applications/opm.desktop"
}

# Limpar arquivos temporários
cleanup() {
    print_info "Limpando arquivos temporários..."
    
    if [ -f "opm" ]; then
        rm -f opm
        print_info "Binário temporário removido"
    fi
}

# Main
main() {
    echo ""
    echo "======================================"
    echo "  Script de Instalação do OPM"
    echo "======================================"
    echo ""
    
    check_root
    check_dependencies
    compile_opm
    install_binary
    create_desktop_file
    cleanup
    
    echo ""
    print_info "Instalação concluída com sucesso!"
    echo ""
    print_info "Para executar o OPM, digite: opm"
    print_info "Ou encontre-o no menu de aplicativos"
    echo ""
}

# Executar main
main
