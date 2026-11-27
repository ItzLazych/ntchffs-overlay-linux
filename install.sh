#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_NAME="ntchffs-overlay"

TARGET_BASE="${HOME}/.config/obs-studio/plugins"
FLATPAK_BASE="${HOME}/.var/app/com.obsproject.Studio/config/obs-studio/plugins"
FORCE_BUILD=0

usage() {
    cat <<EOF
Использование: ./install.sh [опции]

Опции:
  --flatpak       Установить в каталог плагинов Flatpak-версии OBS Studio
  --build         Принудительно пересобрать плагин из исходников
  --prefix DIR    Указать свой каталог plugins (по умолчанию: ~/.config/obs-studio/plugins)
  -h, --help      Показать справку
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --flatpak)
            TARGET_BASE="${FLATPAK_BASE}"
            shift
            ;;
        --build)
            FORCE_BUILD=1
            shift
            ;;
        --prefix)
            TARGET_BASE="${2:?Укажите путь после --prefix}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Неизвестный параметр: $1" >&2
            usage
            exit 1
            ;;
    esac
done

PREBUILT_SO="${SCRIPT_DIR}/${PLUGIN_NAME}/bin/64bit/${PLUGIN_NAME}.so"
DATA_SRC="${SCRIPT_DIR}/${PLUGIN_NAME}/data"
if [[ ! -d "${DATA_SRC}" ]]; then
    DATA_SRC="${SCRIPT_DIR}/data/obs-plugins/${PLUGIN_NAME}"
fi

check_prebuilt_compatible() {
    [[ -f "${PREBUILT_SO}" ]] || return 1
    command -v ldd >/dev/null 2>&1 || return 0
    if ldd "${PREBUILT_SO}" 2>&1 | grep -qE "not found|не найден"; then
        echo "[!] Готовый ${PLUGIN_NAME}.so требует других версий библиотек для вашего дистрибутива:"
        ldd "${PREBUILT_SO}" 2>&1 | grep -E "not found|не найден" || true
        return 1
    fi
    return 0
}

SO_TO_INSTALL=""
if [[ "${FORCE_BUILD}" -eq 0 ]] && check_prebuilt_compatible; then
    echo "[+] Готовый бинарный файл совместим с вашей системой."
    SO_TO_INSTALL="${PREBUILT_SO}"
else
    echo "[*] Собираем ${PLUGIN_NAME}.so из исходников под ваши системные библиотеки..."
    for tool in cmake g++ pkg-config; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            echo "[!] Не найдена утилита '${tool}'. Установите зависимости сборки (см. README.md)." >&2
            exit 1
        fi
    done
    BUILD_DIR="${SCRIPT_DIR}/build-local"
    rm -rf "${BUILD_DIR}"
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
    JOBS="$(nproc 2>/dev/null || echo 4)"
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
    SO_TO_INSTALL="${BUILD_DIR}/${PLUGIN_NAME}.so"
fi

DEST_DIR="${TARGET_BASE}/${PLUGIN_NAME}"
mkdir -p "${DEST_DIR}/bin/64bit" "${DEST_DIR}/data"
cp -f "${SO_TO_INSTALL}" "${DEST_DIR}/bin/64bit/${PLUGIN_NAME}.so"
cp -rf "${DATA_SRC}/." "${DEST_DIR}/data/"

echo
echo "[OK] Плагин установлен в: ${DEST_DIR}"
echo "     При первом запуске OBS автоматически создастся набор пресетов экспорта по умолчанию"
echo "     (Copy, Balanced, Quality, Strong Compression)."
echo "     Перезапустите OBS Studio и настройте горячие клавиши в:"
echo "     Файл -> Настройки -> Горячие клавиши -> Переключить оверлей / Сохранить повтор."
