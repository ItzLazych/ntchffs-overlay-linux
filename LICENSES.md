# Лицензии

Документ описывает лицензии, применимые к **ntchff's Overlay (Linux Fork)** (`ntchffs-overlay`), и сторонним компонентам, которые используются при сборке и работе плагина.

## Лицензия проекта

**ntchff's Overlay**  
Copyright (C) 2026 ntchff (оригинальный проект: <https://github.com/notch4ff4/ntchffs-overlay>)  
Linux-порт (Wayland / X11): <https://github.com/ItzLazych/ntchffs-overlay-linux>

Программный код плагина распространяется на условиях **GNU General Public License v2.0 или более поздней версии** (GPL-2.0-or-later).

Полный текст лицензии: [LICENSE](LICENSE)  
Официальный текст GPL-2.0: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## Зависимости времени выполнения и сборки

Плагин загружается в OBS Studio и использует библиотеки, присутствующие в окружении OBS или в операционной системе Linux.

| Компонент | Назначение | Лицензия | Ссылка |
| --- | --- | --- | --- |
| [OBS Studio](https://obsproject.com/) (`libobs`, `obs-frontend-api`) | API плагинов, интеграция с интерфейсом OBS | GPL-2.0-or-later | <https://github.com/obsproject/obs-studio/blob/master/COPYING> |
| [Qt 6](https://www.qt.io/) (`Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`, `Qt6::Svg`, `Qt6::WaylandClient`) | UI рендеринг, окна настроек и интеграция с Wayland/X11 | LGPL-3.0 / GPL-3.0 (dual license) | <https://www.qt.io/licensing/> |
| [FFmpeg](https://ffmpeg.org/) (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`) | Декодирование и экспорт/перекодирование видео и аудио в галерее | LGPL-2.1-or-later / GPL-2.0-or-later (в зависимости от сборки) | <https://www.ffmpeg.org/legal.html> |
| [Wayland](https://wayland.freedesktop.org/) (`wayland-client`, `wlr-layer-shell`) | Отображение оверлея поверх окон в Wayland-композиторах | MIT | <https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/COPYING> |
| [PulseAudio](https://www.freedesktop.org/wiki/Software/PulseAudio/) (`libpulse-simple`) | Воспроизведение аудио в плеере галереи | LGPL-2.1-or-later | <https://gitlab.freedesktop.org/pulseaudio/pulseaudio/-/blob/master/LICENSE> |
| [X11](https://www.x.org/) (`libX11`, `libXext`, `libXfixes`) | Click-through оверлей и глобальные хоткеи на X11/XWayland | MIT | <https://gitlab.freedesktop.org/xorg/lib/libx11/-/blob/master/COPYING> |

> **Примечание.** Конкретная лицензия FFmpeg и Qt зависит от того, как они собраны в вашем дистрибутиве. При распространении бинарных сборок соблюдайте условия GPL-2.0-or-later и лицензий используемых библиотек.

## Сторонние ресурсы

### Иконки интерфейса

В оверлее используются PNG-иконки из набора [**Phosphor Icons**](https://phosphoricons.com/) (например, `caret-line-left`, `download-simple`, `trash-simple`, `speaker-high`, `chart-line`).

| Ресурс | Лицензия | Ссылка |
| --- | --- | --- |
| Phosphor Icons | MIT | <https://github.com/phosphor-icons/core/blob/main/LICENSE> |

---

## Оригинальный проект и контакты

- Оригинальный автор: **ntchff** (<https://github.com/notch4ff4/ntchffs-overlay>, notch4ff4@gmail.com)
- Linux-форк: <https://github.com/ItzLazych/ntchffs-overlay-linux>

При распространении производных работ или бинарных сборок сохраняйте уведомления об авторских правах и условия лицензий, перечисленных в этом документе.
