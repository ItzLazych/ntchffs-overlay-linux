# ntchffs-overlay (Linux Edition)

Linux-форк (Wayland и X11) плагина **[ntchff's Overlay](https://github.com/notch4ff4/ntchffs-overlay)** для OBS Studio.

> **О создании порта:** этот Linux-порт плагина сделан с помощью моделей **Gemini 4 Argon**, **Gemini 4 Hydrogen** и **Gemini 4 Neon**. Автор не представляет ни малейшего понимания что он там начудил.

Интерактивный оверлей-HUD, галерея повторов, триммер/экспорт видео (Copy / CRF / разрешение / FPS / аудиодорожки / GIF), индикаторы состояния и уведомления для OBS Studio на Linux.

При первом запуске плагин автоматически создаёт стандартные пресеты экспорта:
- `Copy (Lossless Trim)` — быстрая обрезка без перекодирования
- `Balanced` — MP4, CRF 23
- `Quality` — MP4, CRF 18
- `Strong Compression` — MP4, CRF 30, 1280x720

---

## Установка из GitHub Releases (рекомендуется)

1. Скачайте готовый архив со страницы [**Releases**](https://github.com/ItzLazych/ntchffs-overlay-linux/releases):
   - Для Arch Linux / CachyOS / EndeavourOS / Manjaro — `ntchffs-overlay-linux-arch-x86_64.zip`
   - Для Ubuntu 24.04 / производных с аналогичными версиями FFmpeg/Qt6 — `ntchffs-overlay-linux-ubuntu-24.04-x86_64.zip`
2. Распакуйте архив и запустите скрипт установки:
   ```bash
   unzip ntchffs-overlay-linux-*.zip -d ntchffs-overlay-pkg
   cd ntchffs-overlay-pkg
   chmod +x install.sh
   ./install.sh
   ```

Для Flatpak-версии OBS Studio:
```bash
./install.sh --flatpak
```

Что делает `./install.sh`:
1. Проверяет, совместим ли готовый `ntchffs-overlay.so` с библиотеками вашей системы (OBS, Qt6, FFmpeg).
2. Если система совпадает, сразу копирует готовый плагин в `~/.config/obs-studio/plugins/ntchffs-overlay`.
3. Если версии `FFmpeg` / `libobs` / `Qt6` отличаются, автоматически собирает `.so` из включённых в архив исходников под ваши системные библиотеки.

---

## Быстрая установка из исходников с GitHub

Если вы используете другой дистрибутив (Fedora, Debian Testing, openSUSE и др.) или хотите собрать плагин напрямую из репозитория:

```bash
git clone https://github.com/ItzLazych/ntchffs-overlay-linux.git
cd ntchffs-overlay-linux
./install.sh --build
```

Для Flatpak OBS:
```bash
./install.sh --build --flatpak
```

---

## Ручная установка готового плагина

Если вы хотите установить готовый бинарник вручную из релизного архива:

1. Скопируйте папку `ntchffs-overlay` из архива в каталог плагинов OBS:
   - Обычный OBS: `~/.config/obs-studio/plugins/ntchffs-overlay`
   - Flatpak OBS: `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/ntchffs-overlay`
2. Итоговая структура должна выглядеть так:
   ```text
   ~/.config/obs-studio/plugins/ntchffs-overlay/
   ├── bin/64bit/ntchffs-overlay.so
   └── data/
       ├── icons/...
       ├── locale/{en-US.ini,ru-RU.ini}
       └── logo/...
   ```
3. Перезапустите OBS Studio.

> **Важно:** готовые `.so` собраны с системными библиотеками конкретных дистрибутивов (Arch Linux или Ubuntu 24.04). На других дистрибутивах с иными версиями FFmpeg/Qt6/libobs используйте `./install.sh --build` или ручную сборку ниже.

---

## Ручная сборка из исходников

### Зависимости для сборки

- **Arch Linux / CachyOS / EndeavourOS:**
  ```bash
  sudo pacman -S --needed base-devel cmake pkgconf obs-studio ffmpeg qt6-base qt6-svg qt6-wayland wayland-protocols libpulse libx11 libxext libxfixes
  ```
- **Ubuntu / Debian (24.04+):**
  ```bash
  sudo apt update && sudo apt install -y build-essential cmake pkg-config libobs-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev qt6-base-dev qt6-base-private-dev qt6-svg-dev qt6-wayland-dev qt6-wayland-private-dev wayland-protocols libwayland-dev libpulse-dev libx11-dev libxext-dev libxfixes-dev
  ```
- **Fedora:**
  ```bash
  sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config obs-studio-devel ffmpeg-free-devel qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtsvg-devel qt6-qtwayland-devel wayland-protocols-devel wayland-devel pulseaudio-libs-devel libX11-devel libXext-devel libXfixes-devel
  ```

### Команды сборки и установки

```bash
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release
cmake --build build-local -j"$(nproc)"
mkdir -p ~/.config/obs-studio/plugins/ntchffs-overlay/bin/64bit ~/.config/obs-studio/plugins/ntchffs-overlay/data
cp -f build-local/ntchffs-overlay.so ~/.config/obs-studio/plugins/ntchffs-overlay/bin/64bit/
cp -rf data/obs-plugins/ntchffs-overlay/. ~/.config/obs-studio/plugins/ntchffs-overlay/data/
```

---

## Настройка глобальных горячих клавиш на разных окружениях

Сначала назначьте клавиши в **OBS Studio -> Настройки -> Горячие клавиши**:
- **Переключить оверлей** (`obs-plugin-overlay.toggle_overlay`)
- **Сохранить повтор (через оверлей)** (`obs-plugin-overlay.save_replay`)

Как работают глобальные хоткеи в зависимости от вашего окружения:

1. **X11 (KDE X11, XFCE, MATE, Cinnamon, i3, bspwm, GNOME X11)**
   - Работает сразу из коробки через `libX11` (`XQueryKeymap`), даже когда окно OBS не в фокусе.

2. **niri (Wayland)**
   - Плагин автоматически создаёт `~/.config/niri/ntchffs-overlay-binds.kdl` и подключает его в `~/.config/niri/custom.kdl` (или `~/.config/niri/config.kdl`).
   - Глобальные хоткеи, включая сочетания из одних модификаторов (например `Ctrl+Alt`), начинают работать сразу без дополнительных настроек.

3. **Hyprland / Sway / River / KDE Plasma Wayland / GNOME Wayland**
   - **Вариант A (без прав `input` — универсальный триггер):**
     Плагин слушает файл команд `${XDG_RUNTIME_DIR:-/tmp}/ntchffs-overlay.cmd`. Вы можете повесить любое сочетание клавиш в вашем окружении на одну из команд:
     - Показать/скрыть оверлей:
       ```bash
       sh -c 'printf "toggle_overlay\n" >> "${XDG_RUNTIME_DIR:-/tmp}/ntchffs-overlay.cmd"'
       ```
     - Сохранить повтор:
       ```bash
       sh -c 'printf "save_replay\n" >> "${XDG_RUNTIME_DIR:-/tmp}/ntchffs-overlay.cmd"'
       ```
     Примеры:
     - **Hyprland (`~/.config/hypr/hyprland.conf`)**:
       ```ini
       bind = CTRL ALT, O, exec, printf 'toggle_overlay\n' >> "${XDG_RUNTIME_DIR:-/tmp}/ntchffs-overlay.cmd"
       ```
     - **Sway (`~/.config/sway/config`)**:
       ```ini
       bindsym --no-repeat Ctrl+Mod1+o exec sh -c 'printf "toggle_overlay\n" >> "${XDG_RUNTIME_DIR:-/tmp}/ntchffs-overlay.cmd"'
       ```
     - **KDE Plasma / GNOME**: добавьте пользовательскую комбинацию клавиш в «Параметрах системы -> Клавиатура -> Комбинации клавиш» с командой выше.
   - **Вариант B (автоматический перехват через `evdev` для всех Wayland-композиторов):**
     Добавьте пользователя в группу `input` и перелогиньтесь (после этого хоткеи из настроек OBS работают глобально):
     ```bash
     sudo usermod -aG input "$USER"
     ```

4. **Поддержка слоёв оверлея по композиторам**
   - Композиторы с `wlr-layer-shell` (`niri`, `Hyprland`, `Sway`, `River`, `Wayfire`, `Labwc`) отображают оверлей в нативном верхнем overlay-слое с прозрачным кликом сквозь пустые области (для нативной интеграции `wlr-layer-shell` требуется Qt ≥ 6.5 — Arch, Fedora, Debian 13, Ubuntu 24.10+).
   - На **X11** используется `XShape` click-through и `_NET_WM_STATE_ABOVE` (в том числе на Qt 6.4 / Ubuntu 24.04 LTS).
   - На **GNOME Wayland** (`Mutter`, без `wlr-layer-shell`) и на системах Wayland с Qt 6.4 (Ubuntu 24.04 LTS) используется fallback на окно Qt с `wl_surface_set_input_region` click-through.

---

## Лицензия и благодарности

- Оригинальный проект: [notch4ff4/ntchffs-overlay](https://github.com/notch4ff4/ntchffs-overlay) (Copyright (C) 2026 ntchff)
- Распространяется по лицензии **GPL-2.0-or-later** — подробности в [LICENSE](LICENSE) и [LICENSES.md](LICENSES.md).
