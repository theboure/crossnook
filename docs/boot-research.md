# Nook Simple Touch — Исследование загрузки и системы

## 1. Аппаратная платформа

| Компонент | Описание |
|-----------|----------|
| SoC | TI OMAP3621BCYN (ARM Cortex-A8, 800 MHz) |
| RAM | 256 MB DDR (Samsung K4X2G323PC-8GD8) |
| Внутренняя flash | 2 GB eMMC (Samsung KLM2G1HE3F-B001, Sandisk на NST) |
| Дисплей | 6" E-Ink Pearl, 600x800, ED060SCE(LF)C1 |
| Тачскрин | Neonode zForce (ИК, I2C) |
| PMIC (SoC) | TI TPS65921B (USB HS transceiver) |
| PMIC (E-Ink) | TI TPS65181 |
| CPLD | Lattice ispMACH 4032ZE |
| WiFi | Jorjin WG7310-2A (802.11b/g/n) |
| Fuel Gauge | TI BQ27520 |
| MCU | TI MSP430F2272 |
| Модель | BNRV300 |
| Stock OS | Android 2.1 (Eclair) |

## 2. Boot chain с microSD

Nook Simple Touch загружается с microSD по следующей цепочке:

```
ROM Boot (TI OMAP ROM bootloader, 0451:d00e USB ID)
  └─► MLO (x-loader / SPL, First Stage Loader)
        └─► u-boot.bin (вторичный загрузчик)
              └─► boot.script → читает с FAT32 раздела SD:
                    ├── uImage (ядро Linux)
                    └── uRamdisk (initramfs)
                  bootm 0x81c00000 0x81f00000
```

### 2.1. ROM Bootloader (aboot / x-loader)

 OMAP3621 содержит в ROM Boot Loader (TI ABC — Advanced Boot Loader).
 При включении он сканирует microSD на наличие `MLO` (Minimum Loader) в первых секторах.
 Если MLO найден — загружает его. Если нет — переход к eMMC или USB (fastboot/omaplink).

 MLO — это x-loader (SPL — Secondary Program Loader), скомпилированный из
 `nook2_1-2.tgz` → `distro/x-loader/`.

### 2.2. u-boot

 u-boot загружается из MLO. Конфигурация: `omap3621_evt1a_config`.
 Патчена команда boot:

 ```
 #define CONFIG_BOOTCOMMAND "run autodetectmmc; run readtokens; run checkbootcount; run checkrom; run checkupdate; run checkbcb; run ${bootvar}"
 ```

 U-boot загружает boot.script с FAT32 раздела SD:

 ```
 run setbootargs
 mmcinit 0
 mmcinit 1
 fatload mmc 0 0x81c00000 uImage
 fatload mmc 0 0x81f00000 uRamdisk
 bootm 0x81c00000 0x81f00000
 ```

 Адреса загрузки:
 - uImage: `0x81c00000`
 - uRamdisk: `0x81f00000`

### 2.3. Ядро (uImage)

 Загружается как uImage (u-boot image format, `mkimage -A arm -O linux -T kernel`).
 Загрузочный адрес: `0x80008000` (внутри uImage header).

### 2.4. uRamdisk (initramfs)

 Initramfs в формате uImage, содержит Android init.rc.
 Находится в `0x81f00000`.

## 3. SD Image — структура

 NookManager.img — это raw disk image (~64 MiB) с одной FAT32分区:

 ```
 dd if=/dev/zero of=NookManager.img bs=1MiB count=64
 parted → msdos, primary fat32, boot flag
 mkdosfs -F 32 -n "NookManager"
 ```

 Содержимое корня:

 ```
 NookManager.img (FAT32, ~64 MiB)
 ├── MLO                          ← x-loader (из stock firmware 1.2.0 update)
 ├── u-boot.bin                   ← скомпилированный из BN source
 ├── uImage                       ← скомпилированный из BN source
 ├── uRamdisk                     ← сгенерированный buildroot (initramfs)
 ├── boot.scr                     ← скрипт загрузки u-boot
 ├── boot.script                  ← исходник boot.scr (текст)
 ├── booting.pgm                  ← заставка NookManager
 ├── flash_spl.bin                ← из stock firmware
 ├── cfg.bin                      ← из stock firmware
 ├── wvf.bin                      ← из stock firmware (waveform?)
 ├── files/
 │   ├── data/app/                ← APK файлы для установки
 │   ├── system/app/              ← APK для root
 │   ├── system/bin/su            ← superuser binary
 │   ├── system/fonts/            ← замена шрифтов
 │   └── system/framework/        ← патченные JAR файлы
 ├── menu/                        ← пользовательские меню
 ├── scripts/                     ← скрипты (root, backup и т.д.)
 ├── hooks/                       ← хуки
 └── custom/                      ← пользовательские расширения
 ```

## 4. Kernel config

 Ядро: Linux 2.6.29-omap1

 Defconfig: `omap3621_gossamer_evt1c_defconfig`
 (NB: 'gossamer' — кодовое имя платформы NST)

 Источник ядра:
 - BN source: `http://images.barnesandnoble.com/PResources/download/Nook/source-code/nook2_1-2.tgz`
 - GitHub mirror (staylo/nook2): `distro/kernel/`
 - GitLab (rychly/nst-linux-sources): NST Glowlight variant

 Ключевые параметры ядра (из kernel cmdline):
 ```
 androidboot.console=ttyO0 console=ttyO0,115200n8
 mem=448M@0x80000000 mem=512M@0xA0000000
 init=/init root0
 ```

 Примечание: cmdline показывает `root0` — rootfs берётся из initramfs (uRamdisk).

 Кросс-компилятор: CodeSourcery Lite 2010q1 (`arm-none-linux-gnueabi-`)
  или Linaro 4.7. Согласно經驗 — только CodeSourcery 2010q1 работает надёжно.

## 5. Rootfs

### NookManager rootfs

 Генерируется buildroot 2012.08:

 ```
 download_and_extract "http://buildroot.uclibc.org/downloads/buildroot-2012.08.tar.bz2"
 ```

 - uClibc (не glibc)
 - busybox
 - Минимальные Android-утилиты (adb и т.д.)
 - framebuffer вывод через imagemagick
 - Меню и скрипты NookManager

 ### Stock Android rootfs

 На внутренней eMMC (2 GB Sandisk microSD, впаянной):
 ```
 Disk /dev/mmcblk0: 1977 MB
    /dev/mmcblk0p1  boot (FAT32, 78 MB, boot flag)
    /dev/mmcblk0p2  rom (FAT32, 16 MB — серийный ключ, ключи)
    /dev/mmcblk0p3  factory (ext2, 190 MB)
    /dev/mmcblk0p4  nook (FAT32, 240 MB, extended)
    /dev/mmcblk0p5  cache (ext3, 240 MB)
    /dev/mmcblk0p6  data (ext3, 801 MB — Android /data)
    /dev/mmcblk0p7  system (ext3, ~250 MB)
 ```

## 6. Бинарные blobs из stock firmware

 Из скрипта build.sh видно, что следующие компоненты берутся напрямую
 из stock firmware (не компилируются из исходников):

 | Файл | Источник | Назначение |
 |------|----------|------------|
 | `MLO` | nook_1_2_update.zip | x-loader (SPL) — бинарный blob |
 | `flash_spl.bin` | nook_1_2_update.zip | Second Program Loader flash |
 | `cfg.bin` | nook_1_2_update.zip | Конфигурация |
 | `wvf.bin` | nook_1_2_update.zip | Waveform данные для E-Ink? |
 | `su` | su-2.3.6.1-ef-signed.zip | Superuser binary |
 | `android.policy.jar` | nook_1_2_update.zip (патчен) | Android framework |
 | `services.jar` | nook_1_2_update.zip (патчен) | Android framework |
 | `PackageInstaller.apk` | nook_1_1_2_update.zip | Старый package installer |

 Примечание: MLO является Closed Source blob от TI/BN.
 При восстановлении (restoring) NookManager использует их напрямую.
 u-boot и ядро компилируются из исходников BN.

## 7. E-Ink вывод

 ### Дисплей

 E-Ink контроллер: TI TPS65181 (Power Management IC for E Ink Vizplex).
 Панель: ED060SCE(LF)C1 (600x800, Pearl E-Ink).

 ### Framebuffer

 Дисплей отображается через Linux framebuffer:
 - `/dev/graphics/fb0`
 - 16 bpp (RGB565)
 - 600x800

 ### Обновление экрана

 После записи данных в framebuffer необходимо вызвать обновление:
 ```c
 write(fb, "0", 0);  // write с 0 байтами триггерит update
 ```

 Полный e-ink refresh (очистка ghosting):
 ```c
 int refresh = open("/sys/class/graphics/fb0/epd_refresh", O_RDWR);
 write(refresh, "1", 1);
 ```

 ### Kernel driver

 В ядре 2.6.29 BN используется собственный framebuffer driver для OMAP DSS
 с кастомной поддержкой E-Ink через TPS65181.
 Драйвер регистрируется как platform device (`omapfb`).

 driver name: `omapfb`
 sysfs: `/sys/class/graphics/fb0/epd_refresh`

 ### Примечания

 - Нет встроенного DRM — используется fbdev
 - Double-buffer панинг не поддерживается E-Ink драйвером
 - При обновлении через FBIOPUT_VSCREENINFO (yoffset) — page flip
 - SDL 1.2 fbcon требует патча для работы с E-Ink

 ## 8. Кнопки и тачскрин

 ### Ввод (input devices)

 | Устройство | Устройство ввода | Описание |
 |------------|-----------------|----------|
 | Side buttons | `/dev/input/event0` | Кнопки перелистывания (Page Up/Down) |
 | Home & Power | `/dev/input/event1` | Домой и питание |
 | Touchscreen | `/dev/input/event2` | Neonode zForce (I2C) |

 ### Neonode zForce Touchscreen

 - Интерфейс: I2C
 - Протокол: I2C command/response, multi-touch до 2 пальцев
 - Драйвер в Linux: `drivers/input/touchscreen/zforce_ts.c` (upstream с 3.13)
 - В ядре 2.6.29 BN: кастомный I2C драйвер (не upstream)
 - Compatible: `"neonode,zforce"`
 - GPIO: interrupt (IRQ) + reset

 Драйвер upstream (Heiko Stuebner, MundoReader S.L.):
 - Основан на оригинальном BN драйвере (Pieter Truter, Barnes & Noble)
 - Поддерживает multitouch, ABS_MT_POSITION_X/Y
 - I2C команды: INITIALIZE, RESOLUTION, SCANFREQ, SETCONFIG, DATAREQUEST

 ### GPIO кнопки

 Кнопки side (Page) и home — GPIO-based input, подключены к OMAP GPIO.
 Power button — через TPS65921 PMIC (wake source).

 ## 9. Serial Console

 ### UART

 На OMAP3621 доступны UART:
 - `ttyO0` (UART0) — основная консоль, 115200,n81
 - `ttyO1` (UART1)

 Kernel cmdline:
 ```
 androidboot.console=ttyO0 console=ttyO0,115200n8
 ```

 ### Физический доступ

 UART доступен через паяные контакты на плате.
 Для подключения необходим UART-to-USB адаптер (FTDI, 3.3V TTL).
 Необходимо припаять провода к контактам UART на PCB.

 ### U-Boot console

 U-Boot по умолчанию может быть настроен на автозагрузку.
 С patched u-boot возможно獲得 shell (run normalboot для загрузки ОС).

 ### ADB

 ADB доступен по USB ( gadget mode ) или через WiFi (adbkonnect app).
 stock: `androidboot.console=ttyO0` указывает что init/shell идёт на UART0.

 ## 10. Как получить shell / debug output

 ### Простые способы (без пайки)

 1. **NookManager** — записать на SD, загрузиться, NookManager запускает minimal
    Android env с busybox, ADB доступен.

 2. **ADB по USB** — после рута через NookManager, ADB работает:
    ```
    adb shell
    ```

 3. **ADB по WiFi** — NookManager устанавливает ADB Konnect app,
    можно включить wireless ADB.

 4. **Framebuffer напрямую** — из ADB shell:
    ```
    cat /dev/urandom > /dev/graphics/fb0    # шум на экране
    ```

 ### С пайкой

 5. **UART Serial Console** — припаять UART, подключить FTDI USB-serial,
    115200,n81 на ttyO0. Видно boot log ядра, u-boot, init.

 6. **Nokiaomaplink** (omaplink.exe) — USB boot через OMAP ROM bootloader
    (VID:PID 0451:d00e) для восстановления/прошивки.

 ### Для отладки нового ядра/ОС

 7. **Boot from SD, serial console** — лучший вариант:
    - Компилировать своё ядро с `console=ttyO0,115200n8`
    - Подключить UART
    - Загружаться с SD
    - Видеть полный boot log

 8. **Framebuffer write** — если serial недоступен:
    - В initramfs: писать текст на fb0 через framebuffer (rgb565)
    - pixmap/imagemagick approach (как в NookManager)

 ## 11. Build system NookManager

 `build.sh` — основной скрипт сборки (требует 32-bit Linux):

 1. Скачивает исходники BN: `nook2_1-2.tgz` (ядро, u-boot, x-loader)
 2. Скачивает firmware update: `nook_1_2_update.zip` (MLO, blobs)
 3. Скачивает buildroot 2012.08
 4. Конфигурирует buildroot с кастомными настройками
 5. Компилирует buildroot (busybox, coreutils, imagemagick)
 6. Делает `mkimage` для uRamdisk
 7. Компилирует u-boot из BN source (omap3621_evt1a_config)
 8. Компилирует ядро из BN source (omap3621_gossamer_evt1c_defconfig)
 9. Патчит Android JAR (android.policy.jar, services.jar)
 10. Создаёт FAT32 image (NookManager.img)

 Кросс-компилятор: android-ndk-r5 или CodeSourcery 2010q1
 (`arm-linux-androideabi-4.4.3` или `arm-none-linux-gnueabi-`)

 ## 12. Источники

 | Ресурс | URL |
 |--------|-----|
 | NookManager (doozan) | https://github.com/doozan/NookManager |
 | BN Kernel Source (1.2) | http://images.barnesandnoble.com/PResources/download/Nook/source-code/nook2_1-2.tgz |
 | staylo/nook2 | https://github.com/staylo/nook2 |
 | rychly/nst-linux-sources | https://gitlab.com/rychly/nst-linux-sources |
 | felixhaedicke/nst-kernel | https://github.com/felixhaedicke/nst-kernel |
 | mali1/NST-kernel | https://github.com/mali1/NST-kernel |
 | zforce_ts upstream driver | https://github.com/torvalds/linux/blob/master/drivers/input/touchscreen/zforce_ts.c |
 | nookdevs.com wiki | https://nookdevs.com/Nook_Simple_Touch |
 | badd10de notes | https://badd10de.dev/notes/nook-simple-touch.html |
 | XDA NookManager thread | https://xdaforums.com/t/nookmanager-updated-for-1-2-2.3973967/ |
 | XDA NST rooting | https://xdaforums.com/t/nst-manual-rooting-adb-gapps-1-1-etc.1380400/ |
 | NookColor Mer Wiki | https://wiki.merproject.org/wiki/Adaptation/Nook_Color |

 ---

## 13. Неизвестные для проверки на устройстве

 Следующие параметры необходимо проверить на реальном Nook Simple Touch:

 ### Boot и загрузка
 - [ ] Точная версия firmware (1.2.0 / 1.2.1 / 1.2.2)
 - [ ] Работает ли загрузка с microSD без патча u-boot
 - [ ] Содержимое FAT32 partition (boot) на внутренней eMMC — точный список файлов
 - [ ] Есть ли boot.scr на внутреннем boot partition (или только u-boot autodetect)
 - [ ] Формат MLO — проверить, является ли он identical с nook_1_2_update.zip

 ### UART / Serial
 - [ ] Наличие и доступность паяных контактов UART на конкретном PCB revision
 - [ ] Версия UART (115200 — стандартная, но проверить)
 - [ ] Видит ли u-boot UART консоль (выводит ли что-то при загрузке)
 - [ ] Видит ли ядро UART (console=ttyO0 работает ли)

 ### E-Ink / Framebuffer
 - [ ] Точное разрешение framebuffer (ожидается 600x800, 16bpp)
 - [ ] Путь к framebuffer device (ожидается /dev/graphics/fb0)
 - [ ] Работает ли `/sys/class/graphics/fb0/epd_refresh`
 - [ ] Формат framebuffer: rgb565 vs_argb8888
 - [ ] Ограничения обновлений (скорость, ghosting, waveform modes)

 ### Ввод
 - [ ] Точная нумерация input devices (event0/event1/event2)
 - [ ] Коды кнопок (KEY_PAGEUP/PAGEDOWN, KEY_HOME, KEY_POWER)
 - [ ] Работает ли тачскрин без init Android (в чистом Linux)
 - [ ] I2C адрес zForce (ожидается ~0x50)

 ### ADB / USB
 - [ ] Работает ли ADB по USB после рута
 - [ ] USB gadget mode — работает ли без драйверов Android
 - [ ] USB host mode — ограничения по питанию

 ### Питание
 - [ ] Состояние батареи (health, voltage, capacity)
 - [ ] BQ27520 fuel gauge — читается ли через I2C/sysfs
 - [ ] TPS65181 (E-Ink PMIC) — I2C адрес, доступность через sysfs

 ### Внутренняя eMMC
 - [ ] Точное разделение (размеры всех partitions)
 - [ ] Содержимое /rom partition (серийник, ключи — НЕ ЧИТАТЬ на чужом устройстве)
 - [ ] Содержимое /factory partition
 - [ ] Можно ли загрузиться с eMMC после записи пустого SD

 ### Ядро
 - [ ] Точная строка kernel cmdline (проверить через /proc/cmdline)
 - [ ] Количество CPU cores (ожидается 1, несмотря на SMP)
 - [ ] Частоты CPU (300/600/800 MHz)
 - [ ] CONFIG_* опции ( défaut vs custom)

 ### WiFi
 - [ ] Модуль WiFi (ожидается Jorjin WG7310-2A / WL12xx)
 - [ ] Загружается ли wl12xx module

 ### Другое
 - [ ] Контакты JTAG/调试 (если есть)
 - [ ] CPLD (Lattice ispMACH 4032ZE) — функция, прошиваемость
 - [ ] MSP430 MCU — функция (power management, кнопки?)

---

*Документ создан: 2026-09-16*
*Источники: NookManager GitHub, BN source code, nookdevs.com, XDA forums, badd10de.dev*
