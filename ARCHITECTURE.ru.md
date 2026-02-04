# Архитектура

Проект — ESP32 устройство «часы/аудио» с 4x7?сегментным дисплеем (74HC595), Bluetooth A2DP приёмником, локальным SD?плеером, Wi?Fi конфигом/синхронизацией времени и будильниками. Прошивка разбита на модули в `main/`.

## Общая логика
- `app_main.c` загружает NVS/конфиг, инициализирует подсистемы, запускает задачи и выставляет стартовый UI?режим (часы).
- Переключение UI?режимов централизовано в `app_set_ui_mode()` и выполняется через `ui_cmd_task`, чтобы не делать это из ISR/обработчиков ввода.
- Отрисовка выполняется задачей дисплея: по умолчанию время, поверх — оверлеи/анимации/тексты по событиям.

## Основные модули
- `app_main.c`
  - Оркестрация запуска.
  - Хранит глобальный конфиг (`app_config_t`) и состояние UI.
- `app_control.h/c`
  - Общие enum/хелперы режимов (`app_get_ui_mode`, `app_set_ui_mode`, `app_request_ui_mode`).
- `config_store.*`
  - Загрузка/сохранение настроек (громкость, EQ, яркость, будильник, таймзона и т.д.).
- `config_owner.*`
  - Единственный «owner task» для записи `app_config_t`.
  - Все runtime?записи идут через `config_owner_request_update`.
- `clock_time.*`
  - RTC/таймзона и выдача текущего времени для дисплея.

## Дисплей
- `display_74hc595.*`
  - Низкоуровневый драйвер (битбэнг или SPI).
  - Яркость через OE PWM (LEDC) или программный PWM (`esp_timer`).
- `display_ui.*`
  - Базовое время и оверлеи.
  - `display_ui_render()` решает, что показывать: оверлей или время.
- `display_bt_anim.*`
  - Анимации в BT?режиме на основе `audio_spectrum`.
- `ui_display_task.*`
  - Отдельная задача обновления времени/оверлеев.

## Аудио и Bluetooth
- `audio_pcm5102.*`
  - I2S вывод (PCM5102), тон/будильник, громкость.
- `audio_eq.*`
  - 2?полосный shelving?EQ в `audio_i2s_write`.
  - Low shelf @ 150 Гц, High shelf @ 5 кГц, диапазон +/-6 дБ (шкала 0..30, центр=15).
- `bluetooth_sink.*`, `bt_app_av.*`, `bt_app_core.*`, `bt_avrc.*`
  - Инициализация BT, A2DP, AVRCP, ring?buffer и I2S?задача.
- `audio_spectrum.*`
  - Лёгкий 4?полосный визуализатор (не влияет на аудио).

## Память и плеер
- `storage/storage_sd_spi.*`
  - SD по SPI; mount/unmount.
- `audio_player.*`
  - Локальное воспроизведение с SD.

## Сеть и web UI
- `wifi_ntp.*`
  - Жизненный цикл Wi?Fi + синхронизация времени (NTP).
- `web_config.*`
  - Минимальный web?интерфейс настройки Wi?Fi (SSID/пароль/сброс).

## Ввод и UI
- `ui_input.*`
  - Инициализация энкодера/ADC?клавиш.
- `ui_input_handlers.*`
  - Переключение режимов, громкость, управление воспроизведением, меню/установка времени.
- `ui_menu.*`
  - Меню и взаимодействия, включая EQ (`EqUA`).
- `ui_time_setting.*`
  - Режим установки времени.
- `alarm_timer.*`
  - Планировщик будильника.
- `power_manager.*`
  - Питание и soft?power логика.

## API
### Внутренние (C) API
- `app_control.h`: `app_get_ui_mode`, `app_request_ui_mode`, `app_set_ui_mode`.
- `display_74hc595.h`: `display_init`, `display_set_time`, `display_set_text`,
  `display_set_segments`, `display_set_brightness`, `display_get_brightness`.
- `display_ui.h`: `display_ui_init`, `display_ui_set_time`, `display_ui_show_text`,
  `display_ui_show_digits`, `display_ui_show_segments`, `display_ui_render`.
- `display_bt_anim.h`: `display_bt_anim_reset`, `display_bt_anim_update`.
- `audio_pcm5102.h`: `audio_init`, `audio_set_volume`, `audio_i2s_write`,
  `audio_i2s_set_sample_rate`, `audio_play_tone`, `audio_play_alarm`, `audio_stop`.
- `audio_eq.h`: `audio_eq_init`, `audio_eq_set_sample_rate`, `audio_eq_set_steps`,
  `audio_eq_is_flat`, `audio_eq_process`.
- `audio_player.h`: `audio_player_init`, `audio_player_play`, `audio_player_pause`,
  `audio_player_stop`, `audio_player_next`, `audio_player_prev`,
  `audio_player_set_volume`, `audio_player_get_state`, `audio_player_get_time_ms`.
- `bluetooth_sink.h`: `bt_sink_init`, `bt_sink_set_discoverable`, `bt_sink_disconnect`,
  `bt_sink_is_connected`, `bt_sink_is_streaming`, `bt_sink_set_name`,
  `bt_sink_clear_bonds`.
- `bt_avrc.h`: `bt_avrc_send_command`, `bt_avrc_register_volume_cb`,
  `bt_avrc_notify_volume`, `bt_avrc_is_connected`.
- `config_store.h`: `config_store_init`, `config_store_get`, `config_store_update`.
- `config_owner.h`: `config_owner_init`, `config_owner_start`, `config_owner_request_update`.
- `clock_time.h`: `clock_time_init`, `clock_time_get`, `clock_time_set_timezone`.
- `wifi_ntp.h`: `wifi_init`, `wifi_set_enabled`, `wifi_is_enabled`,
  `wifi_update_credentials`.
- `storage_sd_spi.h`: `storage_sd_init`, `storage_sd_unmount`, `storage_sd_is_mounted`.
- `alarm_timer.h`: `alarm_timer_init`, `alarm_set`.
- `power_manager.h`: `power_manager_init`, `power_manager_set_autonomous`,
  `power_manager_handle_boot`.

### Web HTTP API
- `GET /` — редирект на `/wifi`.
- `GET /wifi` — страница Wi?Fi и статус.
- `POST /wifi` — сохранить Wi?Fi (`ssid`, `pass`).
- `POST /wifi_reset` — очистка учётных данных Wi?Fi.
- Для fetch?запросов ответ JSON `{ "ok": true }`.

## Задачи и таймеры
- Задачи:
  - `ui_cmd_task` (переключение режимов)
  - `cfg_owner` (сериализованные записи конфига)
  - `display_task` (рендер UI)
  - `BtAppTask` (диспетчер BT событий)
  - `BtI2STask` (BT ringbuffer -> I2S)
  - `audio_task` (тон/будильник)
  - `web_cfg_stop` (отложенная остановка HTTP)

## Потоки данных
- Ввод (энкодер/ADC) -> `ui_input_handlers` -> `app_request_ui_mode` / громкость / меню / установка времени.
- BT A2DP -> `bt_app_av` -> ring?buffer -> `bt_app_core` -> `audio_i2s_write` (EQ) -> I2S.
- Задача дисплея -> `display_ui` -> `display_74hc595`.
- BT аудио -> `audio_spectrum` -> `display_bt_anim`.

## Вход сборки
- `main/CMakeLists.txt` регистрирует все модули.

## ????????? ?????????
- BT prefetch start is deterministic: playback starts only by byte watermark (`s_prefetch_start_bytes`).
- `BtI2STask` priority is fixed to `9`.
- Runtime logging is reduced: mode switch + heap stay at INFO, transition details are DEBUG.
