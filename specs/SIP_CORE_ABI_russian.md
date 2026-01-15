# Описание документа

Этот документ описывает ABI-интерфейс `sip_core_bindings` и правила взаимодействия клиента с ядром SIP. Секции ниже дают актуальные сигнатуры, параметры и логику работы функций, а также события нотифаеров. Все сигнатуры приведены в виде `имя(параметры): Тип`.

Типы информационных блоков:
- **CHANGES** — пометки о новых возможностях или изменённом поведении. В документе оформляются как отдельный блок с заголовком `CHANGES`.
- **IMPORTANT (WARNING)** — критичные замечания, ограничения или устаревшие методы. В документе оформляются как отдельный блок с заголовком `IMPORTANT (WARNING)`.

Пример оформления:

> **CHANGES**: краткое описание изменения.
>
> **IMPORTANT (WARNING)**: важное предупреждение или ограничение.

# Quickstart

- **WINDOWS**:
	1. Подключить к проекту с помощью любого линковщика две динамически подключаемые библиотеки **sip_core_bindings.dll** и **sip_core.dll**  ;
    2. Вызвать нижеперечисленные методы из C++.

- **LINUX / ANDROID**:
	1. Подключить к проекту с помощью любого линковщика две динамически подключаемые библиотеки: **sip_core_bindings.so** и **sip_core.so**;
    2. Вызвать нижеперечисленные методы из C++.

- **DARWIN / IOS**:
	1. Подключить к проекту с помощью любого линковщика две динамически подключаемые библиотеки (FAT формат, содержат в себе две архитектуры - "arm64" и "x86_64"): **sip_core_bindings.dylib** и **sip_core.dylib**;
    2. Вызвать нижеперечисленные методы из C++.

# Краткая сводка по используемым объектам из фреймворка sip_core_bindings

В библиотеке используется:
- 5 контроллеров:
  - Client - контролирует ядро;
  - CallController - отвечает за все операции со звонками;
  - ConfigurationController - отвечает за операции с аккаунтами, с настройками приложения;
  - PresenceController - отвечает за операции с подписками;
  - VideoController - отвечает за операции с видео;
- 4 нотифаера:
  - CallNotifier - сообщает о событиях, относящиеся к определенным звонкам;
  - ConfigurationNotifier - сообщает о событиях, относящиеся к аккаунтам и конфигурации приложения;
  - PresenceNotifier - сообщает о событиях, относящиеся к подпискам;
  - VideoNotifier - сообщает о событиях, относящиеся к видео.

Контроллеры - это статические классы, используемые для обработки и контроля за некоторыми событиями. Контроллеры должны работать из любого места приложения, обратная ситуация является ошибкой и требует исправления.

Нотифаеры - обычные абстрактные классы, которые должны быть загружены в контроллер Client через метод loadSignals. Ядро направляет "клиенту" свои события через переопределенные методы этих классов. 
Клиент - субъект, который реализовал эти классы.

Все методы даны на логическом языке который используется для генерации связок.

# Client - контролирует ядро:

### `initLibrary(flags: Int): Boolean`
Метод для инициализации библиотеки SIP.
- `flags`: Флаги инициализации. Возможные значения:
  -  0 - тихий запуск;
  -  1 - включить отладку (некоторые ассерты во время выполнения);
  -  2 - включить вывод сообщений в консоль уровня INFO+;
  -  3 - включить 1 + 2 - это значит отладочные сообщения будут отображаться в консоли.

Возвращает `true`, если инициализация была успешна, иначе `false`.

### `startLibrary(configDir: String, dataDir: String?): Boolean`
Метод для запуска библиотеки SIP.
- `configDir`: Путь к директории где будет храниться файл конфигурации sip.yaml. Сюда будет сохраняться сгенирированная приложением конфигурация. Нужен для сохранения (например, выбранных юзером устройства вопроизведения) и кэширования данных, а также для дебага (можно просмотреть, что использует приложение в текущий момент без точек останова и т.д.).
- `dataDir`: Путь к директории с данными. Используется для поиска рингтона. (необходимо в данной папке создать путь ringtones, туда поместить файл default.wav). Если клиент сам будет воспроизводить рингтон, то можно не указывать.

Возвращает `true`, если запуск был успешен, иначе `false`.

### `loadSignals(callNotifier: CallNotifier, configurationNotifier: ConfigurationNotifier, presenceNotifier: PresenceNotifier, videoNotifier: VideoNotifier)`
Метод для загрузки сигналов.
- `callNotifier`: обработчик сигналов вызовов;
- `configurationNotifier`: обработчик сигналов конфигурации;
- `presenceNotifier`: обработчик сигналов присутствия;
- `videoNotifier`: обработчик сигналов видео.

### `exit(): Int`
Метод для завершения работы ядра.
Возвращает целочисленное значение кода выхода. 0 - работа ядра завершена успешно.

### `setFileLogging(path: String)`
Метод для установки ретрансляции всех сообщений (зависит от того, с какими параметрами был вызван метод startLibrary) в файл.  

- `path`: путь к директории, где будет создан sip_core.log. Должна быть доступна для записи.

# CallController - все операции со звонками:

Ниже используются общие термины:
- `callId` — ID вызова.
- `confId` — ID конференции.
- `accountId` — ID учетной записи.
- `MediaMap` — словарь параметров медиа (все значения строковые):
  - `MEDIA_TYPE`: тип медиа (`MEDIA_TYPE_AUDIO` или `MEDIA_TYPE_VIDEO`);
  - `ENABLED`: признак включения медиа в SDP (`true`/`false`);
  - `MUTED`: локальный mute потока (`true`/`false`);
  - `SOURCE`: источник медиа (`NONE`, `CAPTURE_DEVICE`, `DISPLAY`, `FILE`);
  - `LABEL`: строковая метка для отладки;
  - `ON_HOLD`: признак удержания потока (`true`/`false`).

### `registerCallHandlers(handlers: Map<String, Callback>)`
Регистрация обработчиков сигналов, связанных со звонками.
- `handlers`: словарь `имя_сигнала -> CallbackWrapperBase` (создаётся через `exportable_callback`).

Ничего не возвращает.
Логика: проксирует регистрацию через `registerSignalHandlers`.

### `placeCall(accountId: String, to: String): String`
Упрощённый запуск исходящего вызова (устаревший метод).
- `accountId`: ID учетной записи отправителя.
- `to`: SIP URI получателя, например `sip:username@domain`.

Возвращает строку с ID вызова или пустую строку при ошибке.
Логика: помечен как deprecated и вызывает `placeCallWithMedia` с пустым списком медиа.

> **IMPORTANT (WARNING)**: метод помечен как устаревший и оставлен только для совместимости. Используйте `placeCallWithMedia`.

### `placeCallWithMedia(accountId: String, to: String, mediaList: List<MediaMap>): String`
Инициация вызова с указанием списка медиа-потоков.
- `accountId`: ID учетной записи отправителя.
- `to`: SIP URI получателя.
- `mediaList`: список `MediaMap`, описывающий медиа-потоки.

Возвращает строку с ID вызова, либо пустую строку при невалидном адресе.
Логика: если `to` пустой — отменяет вызов; иначе вызывает `Manager::outgoingCall`.

### `getCallDetails(accountId: String, callId: String): Map<String, String>`
Получение подробной информации о вызове.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает словарь с деталями вызова. Ключи могут присутствовать частично в зависимости от типа вызова и состояния медиа:
- `CALL_TYPE`: тип вызова (числовое значение enum);
- `PEER_NUMBER`: SIP URI удалённой стороны;
- `INVITE_BODY`: тело INVITE, если доступно;
- `DISPLAY_NAME`: отображаемое имя удалённой стороны;
- `CALL_STATE`: строковое состояние (`INCOMING`, `RINGING`, `CURRENT`, `HOLD`, и т.д.);
- `CONF_ID`: ID конференции, если вызов в конференции;
- `TIMESTAMP_START`: UNIX-временная метка начала вызова;
- `ACCOUNTID`: ID учетной записи;
- `REGISTERED_NAME`: зарегистрированное имя (если доступно);
- `PEER_HOLDING`: `true`/`false`, удерживает ли удалённая сторона;
- `INVITE_CALL_ID`: Call-ID, связанный с INVITE;
- `AUDIO_MUTED`: `true`/`false`, локально ли выключен аудио-захват;
- `VIDEO_MUTED`: `true`/`false`, локально ли выключен видео-захват;
- `AUDIO_ONLY`: `true`/`false`, есть ли только аудио;
- `PEER_MUTED`: `true`/`false`, признак remote mute;
- `PEER_VOICE`: `true`/`false`, признак голосовой активности удалённой стороны (если доступно);
- `VIDEO_SOURCE`: строка источника видео;
- `AUDIO_CODEC`: имя аудио кодека;
- `AUDIO_SAMPLE_RATE`: частота дискретизации аудио;
- `AUDIO_FRACTION_LOST`, `AUDIO_CUM_LOST_PACKET`, `AUDIO_JITTER`, `AUDIO_EXT_HIGH`, `AUDIO_LSR`, `AUDIO_DLSR`:
  параметры RTCP RR для аудио;
- `AUDIO_SPC`, `AUDIO_SOC`, `AUDIO_TIMESTAMP_MSB`, `AUDIO_TIMESTAMP_LSB`, `AUDIO_TIMESTAMP_RTP`:
  параметры RTCP SR для аудио;
- `AUDIO_BR_EXP`, `AUDIO_BR_MANTIS`: параметры REMB для аудио;
- `VIDEO_CODEC`: имя видео кодека;
- `VIDEO_MIN_BITRATE`, `VIDEO_BITRATE`, `VIDEO_MAX_BITRATE`: битрейты видео;
- `VIDEO_FPS`: текущий FPS при приёме;
- `VIDEO_FRACTION_LOST`, `VIDEO_CUM_LOST_PACKET`, `VIDEO_JITTER`, `VIDEO_EXT_HIGH`, `VIDEO_LSR`, `VIDEO_DLSR`:
  параметры RTCP RR для видео;
- `VIDEO_SPC`, `VIDEO_SOC`, `VIDEO_TIMESTAMP_MSB`, `VIDEO_TIMESTAMP_LSB`, `VIDEO_TIMESTAMP_RTP`:
  параметры RTCP SR для видео;
- `VIDEO_BR_EXP`, `VIDEO_BR_MANTIS`: параметры REMB для видео;
- `SOCKETS`: список сокетов для RTP (может отсутствовать).

Логика: ищет вызов по `accountId`/`callId` и возвращает `Call::getDetails()`.

### `getCallList(accountId: String): List<String>`
Получение списка текущих вызовов.
- `accountId`: ID учетной записи. Если строка пустая, возвращаются все вызовы ядра.

Возвращает список ID вызовов.
Логика: при пустом `accountId` берётся общий список, иначе список аккаунта.

### `currentMediaList(accountId: String, callId: String): List<MediaMap>`
Получение списка текущих медиа-потоков вызова/конференции.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.

Возвращает список `MediaMap` с текущими параметрами потоков.
Логика: если найден вызов — возвращает `call->currentMediaList()`, иначе для конференции — `conf->currentMediaList()`.

### `requestMediaChange(accountId: String, callId: String, mediaList: List<MediaMap>): Boolean`
Запрос изменения параметров медиапотоков.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.
- `mediaList`: новый список медиа-параметров.

Возвращает `true`, если запрос отправлен, иначе `false`.
Логика: ищет вызов/конференцию и вызывает `requestMediaChange`.

### `answerMediaChangeRequest(accountId: String, callId: String, mediaList: List<MediaMap>): Boolean`
Ответ на входящий запрос изменения медиа.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `mediaList`: список параметров медиа; должен совпадать по размеру со списком из запроса.

Возвращает `true`, если ответ принят, иначе `false`.
Логика: вызывает `call->answerMediaChangeRequest`; при исключении возвращает `false`.

### `refuse(accountId: String, callId: String): Boolean`
Отклонение входящего вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true` при успешном отклонении.
Логика: проксирует в `Manager::refuseCall`.

### `accept(accountId: String, callId: String): Boolean`
Принятие входящего вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true` при успешном принятии.
Логика: проксирует в `Manager::answerCall`.

### `acceptWithMedia(accountId: String, callId: String, mediaList: List<MediaMap>): Boolean`
Принятие входящего вызова с явным списком медиа.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `mediaList`: список медиа-параметров.

Возвращает `true` при успешном принятии.
Логика: вызывает `Manager::answerCall` с `mediaList`.

### `hangUp(accountId: String, callId: String): Boolean`
Завершение вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true`, если завершение прошло успешно.
Логика: проксирует в `Manager::hangupCall`.

### `hold(accountId: String, callId: String): Boolean`
Постановка вызова на удержание.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true`, если удержание выполнено.
Логика: проксирует в `Manager::onHoldCall`.

### `unhold(accountId: String, callId: String): Boolean`
Снятие удержания вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true`, если удержание снято.
Логика: проксирует в `Manager::offHoldCall`.

### `muteLocalMedia(accountId: String, callId: String, mediaType: String, mute: Boolean): Boolean`
Локальное отключение медиа-потока (аудио/видео).
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.
- `mediaType`: строка типа медиа (например, `MEDIA_TYPE_AUDIO`/`MEDIA_TYPE_VIDEO`).
- `mute`: `true` — выключить, `false` — включить.

Возвращает `true`, если операция выполнена.
Логика: если найден вызов — `call->muteMedia`; если конференция — `conf->muteLocalHost`.

### `muteRemoteMedia(accountId: String, callId: String, mediaType: String, mute: Boolean): Boolean`
Запрос remote mute для вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `mediaType`: тип медиа (сейчас используется только для логирования).
- `mute`: `true` — выключить, `false` — включить.

Возвращает `true`, если операция выполнена.
Логика: если найден вызов — вызывает `call->peerMuted`.

### `transfer(accountId: String, callId: String, to: String): Boolean`
Перевод вызова на другой адрес.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `to`: целевой SIP URI.

Возвращает `true`, если перевод инициирован.
Логика: проксирует в `Manager::transferCall`.

### `attendedTransfer(accountId: String, callId: String, targetID: String): Boolean`
Активный перевод (attended transfer).
- `accountId`: ID учетной записи.
- `callId`: ID вызова, который переводится.
- `targetID`: ID целевого вызова.

Возвращает `true`, если операция успешно инициирована.
Логика: находит вызов и вызывает `call->attendedTransfer`.

### `joinParticipant(accountId: String, sel_callId: String, account2Id: String, drag_callId: String, attached: Boolean): Boolean`
Создание конференции из двух вызовов.
- `accountId`: ID первого аккаунта.
- `sel_callId`: ID первого вызова.
- `account2Id`: ID второго аккаунта.
- `drag_callId`: ID второго вызова.
- `attached`: `true` — создать прикреплённую конференцию, `false` — отсоединённую.

Возвращает `true`, если операция выполнена.
Логика: проксирует в `Manager::joinParticipant`.

### `createConfFromParticipantList(accountId: String, participants: List<String>)`
Создание конференции из списка участников.
- `accountId`: ID учетной записи.
- `participants`: список peer URI/ID участников.

Ничего не возвращает.
Логика: проксирует в `Manager::createConfFromParticipantList`.

### `setConferenceLayout(accountId: String, confId: String, layout: UInt)`
Установка макета конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.
- `layout`: индекс макета.

Ничего не возвращает.
Логика: если найдена конференция — вызывает `conf->setLayout`, иначе отправляет `confOrder` через вызов.

### `isConferenceParticipant(accountId: String, callId: String): Boolean`
Проверка, является ли вызов участником конференции.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает `true`, если вызов участвует в конференции.
Логика: ищет вызов и возвращает `call->isConferenceParticipant()`.

### `addParticipant(accountId: String, callId: String, account2Id: String, confId: String): Boolean`
Добавление участника в конференцию.
- `accountId`: ID учетной записи инициатора.
- `callId`: ID вызова участника.
- `account2Id`: ID учетной записи участника.
- `confId`: ID конференции.

Возвращает `true`, если добавление выполнено.
Логика: проксирует в `Manager::addParticipant`.

### `addMainParticipant(accountId: String, confId: String): Boolean`
Добавление основного участника (локального) в конференцию.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает `true`, если операция выполнена.
Логика: проксирует в `Manager::addMainParticipant`.

### `detachLocalParticipant(): Boolean`
Отсоединение локального участника от конференции.

Возвращает `true` при успехе.
Логика: проксирует в `Manager::detachLocalParticipant`.

### `detachParticipant(accountId: String, callId: String): Boolean`
Отсоединение участника от конференции.
- `accountId`: ID учетной записи (не используется внутри).
- `callId`: ID вызова участника.

Возвращает `true` при успехе.
Логика: проксирует в `Manager::detachParticipant`.

### `joinConference(accountId: String, sel_confId: String, account2Id: String, drag_confId: String): Boolean`
Объединение двух конференций.
- `accountId`: ID учетной записи первого хоста.
- `sel_confId`: ID первой конференции.
- `account2Id`: ID учетной записи второго хоста.
- `drag_confId`: ID второй конференции.

Возвращает `true`, если объединение выполнено.
Логика: проксирует в `Manager::joinConference`.

### `hangUpConference(accountId: String, confId: String): Boolean`
Завершение конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает `true` при успехе.
Логика: проксирует в `Manager::hangupConference`.

### `holdConference(accountId: String, confId: String): Boolean`
Постановка конференции на удержание.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает `true` при успехе.
Логика: проксирует в `Manager::holdConference`.

### `unholdConference(accountId: String, confId: String): Boolean`
Снятие удержания конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает `true` при успехе.
Логика: проксирует в `Manager::unHoldConference`.

### `getConferenceList(accountId: String): List<String>`
Получение списка конференций аккаунта.
- `accountId`: ID учетной записи.

Возвращает список ID конференций.
Логика: возвращает список конференций аккаунта или пустой список.

### `getParticipantList(accountId: String, confId: String): List<String>`
Получение списка участников конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает список идентификаторов участников (peer URI/ID).
Логика: читает `conf->getParticipantList()`.

### `moveParticipant(accountId: String, confId: String, from: UInt, to: UInt): Boolean`
Перемещение участника внутри раскладки по позициям.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `from`: индекс источника в микшере (size_t).
- `to`: целевой индекс (size_t).

Возвращает `true`, если перемещение поддержано, иначе `false`.
Логика: работает только при наличии video mixer; вызывает `conf->moveParticipant(from, to)`.

> **CHANGES**: функция добавлена для управления порядком отображения в конференции.

### `moveParticipant(accountId: String, confId: String, participant_id: String, to: UInt): Boolean`
Перемещение участника по идентификатору.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `participant_id`: идентификатор участника.
- `to`: целевой индекс (size_t).

Возвращает `false` (текущая реализация заглушка).
Логика: на данный момент возвращает `false`, полноценная логика перемещения по ID не реализована.

> **CHANGES**: функция добавлена в ABI, но ещё не реализована в ядре.

### `getConferenceId(accountId: String, callId: String): String`
Получение ID конференции для вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.

Возвращает ID конференции или пустую строку.
Логика: ищет вызов и возвращает `conf->getConfId()` если вызов в конференции.

### `getConferenceDetails(accountId: String, confId: String): Map<String, String>`
Получение деталей конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.

Возвращает словарь:
- `ID`: ID конференции;
- `STATE`: состояние (`ACTIVE_ATTACHED`, `ACTIVE_DETACHED`, `HOLD`);
- `VIDEO_SOURCE`: источник видео (если `ENABLE_VIDEO`);
- `RECORDING`: `true`/`false`;
- `LAYOUT`: индекс текущего макета.

Логика: возвращает состояние конференции, если она найдена.

### `getConferenceInfos(accountId: String, confId: String): List<Map<String, String>>`
Получение информации об участниках конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.

Возвращает список словарей участников:
- `uri`, `device`, `sinkId`, `callId`;
- `active`, `videoMuted`, `audioLocalMuted`, `audioModeratorMuted`, `isModerator`, `handRaised`, `voiceActivity`, `recording` (`true`/`false`);
- `x`, `y`, `w`, `h` — координаты и размеры ячеек.

Логика: возвращает `conf->getConferenceInfos()` либо `call->getConferenceInfos()`.

### `setModerator(accountId: String, confId: String, accountUri: String, state: Boolean)`
Назначение модератора конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `accountUri`: URI участника.
- `state`: `true` — назначить, `false` — снять.

Ничего не возвращает.
Логика: вызывает `conf->setModerator` если конференция найдена.

### `muteParticipant(accountId: String, confId: String, accountUri: String, state: Boolean)`
Отключение аудио участника (устаревший метод).
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `accountUri`: URI участника.
- `state`: `true` — выключить, `false` — включить.

Ничего не возвращает.
Логика: для локальной конференции вызывает `conf->muteParticipant`, иначе отправляет `confOrder`.

> **IMPORTANT (WARNING)**: метод устарел. Используйте `muteStream`.

### `muteStream(accountId: String, confId: String, accountUri: String, deviceId: String, streamId: String, state: Boolean)`
Отключение конкретного медиа-потока в конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.
- `accountUri`: URI участника.
- `deviceId`: ID устройства участника.
- `streamId`: ID потока.
- `state`: `true` — выключить, `false` — включить.

Ничего не возвращает.
Логика: для локальной конференции вызывает `conf->muteStream`; иначе отправляет `confOrder` с учётом версии протокола конференции.

### `setActiveParticipant(accountId: String, confId: String, callId: String)`
Установка активного участника (устаревший метод).
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `callId`: ID участника.

Ничего не возвращает.
Логика: вызывает `conf->setActiveParticipant` либо отправляет `confOrder`.

> **IMPORTANT (WARNING)**: метод устарел. Используйте `setActiveStream`.

### `setActiveStream(accountId: String, confId: String, accountUri: String, deviceId: String, streamId: String, state: Boolean)`
Установка активного потока.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.
- `accountUri`: URI участника.
- `deviceId`: ID устройства участника.
- `streamId`: ID потока.
- `state`: `true` — сделать активным, `false` — сбросить активный.

Ничего не возвращает.
Логика: для локальной конференции вызывает `conf->setActiveStream`, иначе — `SIPCall::setActiveMediaStream`.

### `hangupParticipant(accountId: String, confId: String, accountUri: String, deviceId: String)`
Принудительное завершение участника.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.
- `accountUri`: URI участника.
- `deviceId`: ID устройства участника.

Ничего не возвращает.
Логика: для локальной конференции вызывает `conf->hangupParticipant`, иначе отправляет `confOrder` с учётом версии протокола.

### `raiseParticipantHand(accountId: String, confId: String, peerId: String, state: Boolean)`
Поднятие руки участника (устаревший метод).
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `peerId`: ID участника.
- `state`: `true` — поднять, `false` — опустить.

Ничего не возвращает.
Логика: для локальной конференции ищет устройство участника и выставляет `handRaised`, иначе отправляет `confOrder`.

> **IMPORTANT (WARNING)**: метод устарел. Используйте `raiseHand`.

### `raiseHand(accountId: String, confId: String, accountUri: String, deviceId: String, state: Boolean)`
Поднятие руки в конференции.
- `accountId`: ID учетной записи.
- `confId`: ID конференции или вызова-хоста.
- `accountUri`: URI участника (может быть пустым — будет использован username аккаунта).
- `deviceId`: ID устройства.
- `state`: `true` — поднять, `false` — опустить.

Ничего не возвращает.
Логика: для локальной конференции выставляет `handRaised`; для удалённой отправляет `confOrder` (протокол v1/v0).

### `startSmartInfo(refreshTimeMs: UInt)`
Устаревший метод статистики.
- `refreshTimeMs`: период в миллисекундах.

Ничего не возвращает.
Логика: метод помечен как deprecated и не выполняет действий.

> **IMPORTANT (WARNING)**: метод не работает и оставлен только для совместимости.

### `stopSmartInfo()`
Остановка статистики (deprecated).

Ничего не возвращает.
Логика: метод помечен как deprecated и не выполняет действий.

### `startRecordedFilePlayback(filepath: String): Boolean`
Запуск воспроизведения записанного файла.
- `filepath`: путь к файлу.

Возвращает `true`, если воспроизведение удалось запустить.
Логика: проксирует в `Manager::startRecordedFilePlayback`.

### `stopRecordedFilePlayback()`
Остановка воспроизведения записанного файла.

Ничего не возвращает.
Логика: проксирует в `Manager::stopRecordedFilePlayback`.

### `toggleRecording(accountId: String, callId: String): Boolean`
Переключение записи вызова/конференции.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.

Возвращает `true`, если запись активирована после переключения.
Логика: проксирует в `Manager::toggleRecordingCall`.

### `setRecording(accountId: String, callId: String)`
Устаревшая оболочка над `toggleRecording`.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.

Ничего не возвращает.
Логика: вызывает `toggleRecording`.

### `recordPlaybackSeek(value: Double)`
Перемотка воспроизведения записи.
- `value`: позиция в процентах (0–100).

Ничего не возвращает.
Логика: проксирует в `Manager::recordingPlaybackSeek`.

### `getIsRecording(accountId: String, callId: String): Boolean`
Проверка записи вызова/конференции.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.

Возвращает `true`, если запись активна.
Логика: для вызова — `call->isRecording()`, для конференции — `conf->isRecording()`.

### `playDTMF(accountId: String, callId: String, dtmfEvents: String, duration: Double, volume: UInt)`
Отправка DTMF в рамках вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `dtmfEvents`: строка с последовательностью символов DTMF.
- `duration`: длительность для RTP DTMF.
- `volume`: громкость для RTP DTMF.

Ничего не возвращает.
Логика: вызывает `call->carryingDTMFdigits`; для SIP INFO длительность берётся из настроек, для RTP используется `duration`.

### `startTone(start: Int, type: Int)`
Старт/остановка тонов ядра.
- `start`: `1` — запустить тон, `0` — остановить.
- `type`: тип тона (`0` — обычный, `1` — с сообщением).

Ничего не возвращает.
Логика: вызывает `Manager::playTone`/`playToneWithMessage` или `stopTone`.

### `switchInput(accountId: String, callId: String, resource: String): Boolean`
Переключение видео-источника для вызова или конференции.
- `accountId`: ID учетной записи.
- `callId`: ID вызова или конференции.
- `resource`: строка ресурса в формате MRL `<prefix>://<suffix>`.

Возвращает `true`, если переключение инициировано.
Логика: если найден вызов/конференция — вызывает `switchInput(resource)`.

> **CHANGES**: добавлена поддержка display-URI.
> Префиксы и разбор:
> - `camera://<id>` — устройство камеры;
> - `file://<path>` — файл;
> - `display://<name>` — захват экрана. После `display://` берётся суффикс и:
>   - macOS: `initAVFoundation` (screen capture через AVFoundation);
>   - Windows: `initScreenCaptureRecorder` (если включён `USE_DSHOW_SCREEN_CAPTURE`) или `initWindowsCapture`;
>   - Linux/Unix: `initX11` (имя X11 дисплея).
> Пустая строка `resource` выключает текущий ввод.

Детали формата `display://<suffix>` (как передать номер дисплея, ID окна и область захвата):
- **Linux/Unix (X11)**:
  - `suffix` передаётся как X11 display string, а размер области можно добавить после пробела.
  - Примеры:
    - Полный экран: `display://:0`
    - Полный экран с позицией: `display://:1+0,0 2560x1440`
    - Область экрана: `display://:1+882,211 1532x779`
    - Окно по ID: `display://:+1,0 0x0 window-id:0x0340021e`
  - Если присутствует `window-id:<hex>`, захват идёт по HWND/ID окна, а параметры области (`WxH`) игнорируются.
  - Ширина/высота округляются вниз до кратности 8.
- **Windows (USE_DSHOW_SCREEN_CAPTURE)**:
  - Используется screen-capture-recorder, параметры указываются в `suffix`.
  - `source:<id>` задаёт источник:
    - `source:0`, `source:1`, ... — номер дисплея (нумерация с нуля);
    - `source:0x0340021e` — HWND окна (hex).
  - Дополнительно можно указать размер и смещение:
    - `<W>x<H>` — размер области;
    - `+<X>x<Y>` — смещение области.
  - Примеры:
    - `display://1920x1080 source:0`
    - `display://1920x1080 +28x28 source:1`
    - `display://source:0x0340021e`
  - Ширина/высота округляются вниз до кратности 8.
- **Windows (без USE_DSHOW_SCREEN_CAPTURE)**:
  - Формат такой же, `source:` кладётся в `window_id` и может быть `0x...` или номер дисплея.
  - Пример: `display://source:0 1280x720 +100x50`.
  - Ширина/высота округляются вниз до кратности 8.
- **macOS (AVFoundation)**:
  - Используется источник `Capture screen 0`. Номер дисплея сейчас не задаётся.
  - Можно передать размер после пробела: `display://screen 1920x1080` или `display://0 1920x1080`.
  - Если размер не задан — используются значения по умолчанию.
  - Ширина/высота округляются вниз до кратности 8.

### `switchSecondaryInput(accountId: String, confId: String, resource: String): Boolean`
Устаревший метод переключения дополнительного источника.
- `accountId`: ID учетной записи.
- `confId`: ID конференции.
- `resource`: строка ресурса.

Всегда возвращает `false`.
Логика: метод помечен как deprecated и ничего не делает (логирует ошибку).

> **IMPORTANT (WARNING)**: используйте `requestMediaChange`.

### `sendTextMessage(accountId: String, callId: String, messages: Map<String, String>, from: String, isMixed: Boolean)`
Отправка текстового сообщения в рамках вызова.
- `accountId`: ID учетной записи.
- `callId`: ID вызова.
- `messages`: словарь payload (`mime-type -> текст`).
- `from`: строка отправителя (передаётся ядру, может быть проигнорирована на текущей версии).
- `isMixed`: признак смешанного сообщения.

Ничего не возвращает.
Логика: выполняет отправку через `Manager::sendCallTextMessage` на главном потоке.

# ConfigurationController - операции с аккаунтами, с настройками приложения:

### `registerConfHandlers(handlers: Map<String, Callback>)`
Регистрация обработчиков конфигурационных сигналов.
- `handlers`: словарь `имя_сигнала -> CallbackWrapperBase`.

Ничего не возвращает.
Логика: проксирует в `registerSignalHandlers`.

### `getKeepAliveInterval(accountId: String): Int`
Получение интервала keep-alive для аккаунта.
- `accountId`: ID учетной записи.

Возвращает значение в секундах.
Логика: `Manager::getKeepAliveInterval(accountId)`.

### `setKeepAliveInterval(accountId: String, interval: Int)`
Установка интервала keep-alive.
- `accountId`: ID учетной записи.
- `interval`: интервал в секундах (0 — отключить).

Ничего не возвращает.
Логика: `Manager::setKeepAliveInterval`.

### `registerEventPackage(eventPackage: String, expires: Int): Boolean`
Регистрация SIP event package.
- `eventPackage`: имя пакета.
- `expires`: срок подписки в секундах.

Возвращает `true`, если регистрация успешна.
Логика: проксирует в `Manager::registerEventPackage`.

### `setWebRtcParams(params: WebRtcParams)`
Установка параметров WebRTC DSP.
- `params`: структура со значениями `targetLevelDbfs`, `compressionGainDb`, `limiter`, `experimentalNs`, `noiseGen`.

Ничего не возвращает.
Логика: `Manager::setWebRtcParams`.

### `getWebRtcParams(): WebRtcParams`
Получение текущих параметров WebRTC DSP.

Возвращает структуру `WebRtcParams`.
Логика: `Manager::getWebRtcParams`.

### `getAccountDetails(accountID: String): Map<String, String>`
Получение полной конфигурации аккаунта.
- `accountID`: ID учетной записи.

Возвращает словарь настроек аккаунта. Ключи соответствуют `Account::ConfProperties`:
- `Account.id` — ID аккаунта;
- `Account.type` — тип аккаунта;
- `Account.alias` — псевдоним;
- `Account.displayName` — отображаемое имя;
- `Account.enable` — активность;
- `Account.mailbox` — голосовая почта;
- `Account.dtmfType` — тип DTMF;
- `Account.autoAnswer` — автоответ;
- `Account.sendReadReceipt` — отправка подтверждений прочтения;
- `Account.rendezVous` — режим rendezvous;
- `Account.activeCallLimit` — лимит активных вызовов;
- `Account.hostname` — домен/сервер;
- `Account.username` — имя пользователя;
- `Account.bindAddress` — локальный адрес привязки;
- `Account.routeset` — route set;
- `Account.password` — пароль;
- `Account.realm` — realm;
- `Account.localInterface` — сетевой интерфейс;
- `Account.publishedSameAsLocal` — публикация локального адреса;
- `Account.localPort` — локальный порт;
- `Account.publishedPort` — опубликованный порт;
- `Account.publishedAddress` — опубликованный адрес;
- `Account.useragent` — User-Agent;
- `Account.upnpEnabled` — UPNP;
- `Account.hasCustomUserAgent` — кастомный User-Agent;
- `Account.allowCertFromHistory`, `Account.allowCertFromContact`, `Account.allowCertFromTrusted` — политика сертификатов;
- `Account.archivePassword`, `Account.archiveHasPassword`, `Account.archivePath`, `Account.archivePIN` — параметры архива;
- `Account.deviceID`, `Account.deviceName` — ID/имя устройства;
- `Account.proxyEnabled`, `Account.proxyServer`, `Account.proxyPushToken` — proxy/push параметры;
- `Account.peerDiscovery`, `Account.accountDiscovery`, `Account.accountPublish` — discovery/publish параметры;
- `Account.managerUri`, `Account.managerUsername` — параметры менеджера;
- `Account.bootstrapListUrl`, `Account.dhtProxyListUrl` — URL bootstrap/DHT proxy;
- `Account.defaultModerators`, `Account.localModeratorsEnabled`, `Account.allModeratorsEnabled` — модераторы;
- `Account.allowIPAutoRewrite` — авто-перезапись IP;
- `Account.transport` — транспорт SIP;
- `Account.audioPortMin`, `Account.audioPortMax` — аудио порты;
- `Account.videoEnabled`, `Account.videoPortMin`, `Account.videoPortMax` — видео параметры;
- `Account.presenceEnabled`, `Account.presencePublishSupported`, `Account.presenceSubscribeSupported` — presence параметры;
- `Account.registrationExpire`, `Account.registrationStatus` — параметры регистрации;
- `Account.ringtonePath`, `Account.ringtoneEnabled` — рингтон;
- `SRTP.keyExchange`, `SRTP.enable`, `SRTP.rtpFallback` — SRTP параметры.

Логика: возвращает `Manager::getAccountDetails`.

### `getVolatileAccountDetails(accountID: String): Map<String, String>`
Получение «летучих» параметров аккаунта (статусы, регистрация, транспорт).
- `accountID`: ID учетной записи.

Возвращает словарь с ключами из `Account::VolatileProperties`:
- `Account.active`, `Account.deviceAnnounced`, `Account.registeredName`;
- `Account.registrationStatus`, `Account.registrationCode`, `Account.registrationDescription`;
- `Transport.statusCode`, `Transport.statusDescription`.

Логика: возвращает `Manager::getVolatileAccountDetails`.

### `switchTransport(accountID: String, transportType: TransportType): Boolean`
Смена транспорта SIP.
- `accountID`: ID учетной записи.
- `transportType`: `TCP`, `UDP` или `TLS`.

Возвращает `true`, если переключение удалось.
Логика: `Manager::switchTransport`.

### `setAccountDetails(accountID: String, details: Map<String, String>)`
Установка параметров аккаунта.
- `accountID`: ID учетной записи.
- `details`: словарь с ключами из `Account::ConfProperties`.

Ничего не возвращает.
Логика: применяет параметры через `Manager::setAccountDetails` (может потребовать перерегистрацию).

### `setAccountActive(accountID: String, active: Boolean, shutdownConnections: Boolean = false)`
Активировать/деактивировать аккаунт.
- `accountID`: ID учетной записи.
- `active`: `true` — активировать, `false` — выключить.
- `shutdownConnections`: `true` — дополнительно закрыть соединения.

Ничего не возвращает.
Логика: проксирует в `Manager::setAccountActive`.

### `getAccountTemplate(accountType: String): Map<String, String>`
Получение шаблона настроек аккаунта.
- `accountType`: тип аккаунта (сейчас поддерживается `SIP`).

Возвращает шаблонный словарь параметров (ключи как в `getAccountDetails`), либо пустой словарь для неизвестного типа.
Логика: для `SIP` возвращает `SipAccountConfig().toMap()`.

### `addAccount(details: Map<String, String>, accountID: String = ""): String`
Добавление новой учетной записи.
- `details`: параметры аккаунта.
- `accountID`: опциональный ID (может быть пустым для автогенерации).

Возвращает ID созданного аккаунта.
Логика: проксирует в `Manager::addAccount`.

### `monitor(continuous: Boolean)`
Запуск/остановка мониторинга состояния.
- `continuous`: `true` — непрерывный мониторинг, `false` — однократный.

Ничего не возвращает.
Логика: проксирует в `Manager::monitor`.

### `removeAccount(accountID: String)`
Удаление учетной записи.
- `accountID`: ID удаляемой записи.

Ничего не возвращает.
Логика: удаляет аккаунт через `Manager::removeAccount` (с `flush = true`).

### `playDigitSound(digit: String)`
Воспроизведение локального DTMF-сигнала.
- `digit`: символ DTMF.

Ничего не возвращает.
Логика: вызывает `Manager::playDtmf`.

### `getAccountList(): List<String>`
Получение списка всех аккаунтов.

Возвращает список ID аккаунтов.
Логика: возвращает `Manager::getAccountList`.

### `sendRegister(accountID: String, enable: Boolean)`
Управление регистрацией аккаунта.
- `accountID`: ID учетной записи.
- `enable`: `true` — зарегистрировать, `false` — снять регистрацию.

Ничего не возвращает.
Логика: проксирует в `Manager::sendRegister`.

### `registerAllAccounts()`
Регистрация всех аккаунтов.

Ничего не возвращает.
Логика: проксирует в `Manager::registerAccounts`.

### `sendAccountTextMessage(accountID: String, to: String, payloads: Map<String, String>): ULong`
Отправка сообщения от имени аккаунта (вне вызова).
- `accountID`: ID учетной записи.
- `to`: SIP URI получателя.
- `payloads`: словарь `mime-type -> текст`.

Возвращает ID сообщения (ULong).
Логика: проксирует в `Manager::sendTextMessage`.

### `cancelMessage(accountID: String, messageId: ULong): Boolean`
Отмена отправки сообщения.
- `accountID`: ID учетной записи.
- `messageId`: ID сообщения.

Возвращает `true` при успешной отмене.
Логика: вызывает `acc->cancelMessage`.

### `getLastMessages(accountID: String, base_timestamp: ULong): List<Message>`
Получение последних сообщений.
- `accountID`: ID учетной записи.
- `base_timestamp`: timestamp, начиная с которого запрашиваются сообщения.

Возвращает список `Message` (`from`, `payloads`, `received`).
Логика: возвращает `acc->getLastMessages`, иначе пустой список.

### `getMessageStatus(messageId: ULong): Int`
Получение статуса сообщения по ID.
- `messageId`: ID сообщения.

Возвращает целочисленный статус (`Account::MessageStates`):
- `0` — `UNKNOWN`;
- `1` — `SENDING`;
- `2` — `SENT`;
- `3` — `DISPLAYED`;
- `4` — `FAILURE`;
- `5` — `CANCELLED`.
Логика: `Manager::getMessageStatus`.

### `getMessageStatus(accountID: String, messageId: ULong): Int`
Получение статуса сообщения в рамках аккаунта.
- `accountID`: ID учетной записи.
- `messageId`: ID сообщения.

Возвращает целочисленный статус (`Account::MessageStates`):
- `0` — `UNKNOWN`;
- `1` — `SENDING`;
- `2` — `SENT`;
- `3` — `DISPLAYED`;
- `4` — `FAILURE`;
- `5` — `CANCELLED`.
Логика: `Manager::getMessageStatus(accountID, id)`.

### `applicationProxy(): String`
Получение адреса application proxy.

Возвращает строку адреса.
Логика: возвращает `Manager::applicationProxy`.

### `setIsComposing(accountID: String, conversationUri: String, isWriting: Boolean)`
Установка статуса «печатает».
- `accountID`: ID учетной записи.
- `conversationUri`: URI собеседника/диалога.
- `isWriting`: `true` — печатает, `false` — нет.

Ничего не возвращает.
Логика: вызывает `acc->setIsComposing`.

### `setMessageDisplayed(accountID: String, conversationUri: String, messageId: String, status: Int): Boolean`
Подтверждение отображения сообщения.
- `accountID`: ID учетной записи.
- `conversationUri`: URI диалога.
- `messageId`: ID сообщения.
- `status`: числовой статус отображения.

Возвращает `true`, если статус обновлён.
Логика: вызывает `acc->setMessageDisplayed`.

### `getCodecList(): List<UInt>`
Получение списка доступных кодеков.

Возвращает список ID кодеков.
Логика: читает системный контейнер кодеков, при отсутствии эмитит `ConfigurationSignal::Error`.

### `getCodecDetails(accountID: String, codecId: UInt): Map<String, String>`
Получение параметров кодека.
- `accountID`: ID учетной записи.
- `codecId`: ID кодека.

Возвращает словарь с параметрами `CodecInfo.*` (имя, тип, sampleRate, frameRate, bitrate, min/max bitrate, quality, min/max quality, channelNumber, autoQualityEnabled).
Логика: ищет кодек в аккаунте, возвращает спецификации аудио/видео кодека.

### `setCodecDetails(accountID: String, codecId: UInt, details: Map<String, String>): Boolean`
Изменение параметров кодека.
- `accountID`: ID учетной записи.
- `codecId`: ID кодека.
- `details`: словарь новых параметров.

Возвращает `true`, если параметры применены.
Логика: обновляет параметры кодека и эмитит `MediaParametersChanged`; при активном видео кодеке может перезапустить отправку.

### `getActiveCodecList(accountID: String): List<UInt>`
Получение списка активных кодеков аккаунта.
- `accountID`: ID учетной записи.

Возвращает список ID активных кодеков.
Логика: возвращает активные кодеки аккаунта или дефолтный список.

### `setActiveCodecList(accountID: String, list: List<UInt>)`
Установка списка активных кодеков.
- `accountID`: ID учетной записи.
- `list`: упорядоченный список ID кодеков.

Ничего не возвращает.
Логика: сохраняет порядок и активные кодеки в аккаунте, затем сохраняет конфиг.

### `setDND(accountID: String, isDND: Boolean)`
Установка режима «не беспокоить».
- `accountID`: ID учетной записи.
- `isDND`: `true` — включить, `false` — выключить.

Ничего не возвращает.
Логика: вызывает `acc->setDND`.

### `getAudioPluginList(): List<String>`
Список аудио-плагинов (**LINUX ONLY**).

Возвращает список (`PCM_DEFAULT`, `PCM_DMIX_DSNOOP`).
Логика: возвращает предопределённый список.

### `setAudioPlugin(audioPlugin: String)`
Установка аудио-плагина (Linux).
- `audioPlugin`: имя плагина.

Ничего не возвращает.
Логика: `Manager::setAudioPlugin`.

### `getAudioOutputDeviceList(): List<String>`
Получение списка устройств вывода.

Возвращает список строковых имен устройств.
Логика: `Manager::getAudioOutputDeviceList`.

### `getAudioInputDeviceList(): List<String>`
Получение списка устройств ввода.

Возвращает список строковых имен устройств.
Логика: `Manager::getAudioInputDeviceList`.

### `setAudioOutputDevice(index: Int)`
Выбор устройства вывода.
- `index`: индекс устройства.

Ничего не возвращает.
Логика: `Manager::setAudioDevice(index, PLAYBACK)`.

### `setAudioInputDevice(index: Int)`
Выбор устройства ввода.
- `index`: индекс устройства.

Ничего не возвращает.
Логика: `Manager::setAudioDevice(index, CAPTURE)`.

### `setAudioRingtoneDevice(index: Int)`
Выбор устройства рингтона.
- `index`: индекс устройства.

Ничего не возвращает.
Логика: `Manager::setAudioDevice(index, RINGTONE)`.

### `startAudio()`
Запуск аудио-подсистемы.

Ничего не возвращает.
Логика: `Manager::startAudio`.

### `getCurrentAudioDevicesIndex(): List<Int>`
Получение текущих индексов устройств.

Возвращает список из трёх значений: вывод, ввод, рингтон.
Логика: `Manager::getCurrentAudioDevicesIndex`.

### `getAudioInputDeviceIndex(name: String): Int`
Получение индекса устройства ввода по имени.
- `name`: имя устройства.

Возвращает индекс устройства.
Логика: `Manager::getAudioInputDeviceIndex`.

### `getAudioOutputDeviceIndex(name: String): Int`
Получение индекса устройства вывода по имени.
- `name`: имя устройства.

Возвращает индекс устройства.
Логика: `Manager::getAudioOutputDeviceIndex`.

### `getCurrentAudioOutputPlugin(): String`
Получение текущего аудио-плагина.

Возвращает строку имени плагина.
Логика: `Manager::getCurrentAudioOutputPlugin`.

### `setAudioProcessor(processor: String)`
Установка аудио-процессора.
- `processor`: `null`, `webrtc`, `speex`.

Ничего не возвращает.
Логика: `Manager::setAudioProcessor`.

### `getAudioProcessor(): String`
Получение активного аудио-процессора.

Возвращает строку (`null`, `webrtc`, `speex`).
Логика: `Manager::getAudioProcessor`.

### `getNoiseSuppressState(): String`
Получение режима шумоподавления.

Возвращает `audioprocessor`, `native` или `off`.
Логика: `Manager::getNoiseSuppressState`.

### `setNoiseSuppressState(state: String)`
Установка режима шумоподавления.
- `state`: `audioprocessor`, `native`, `off`, `auto`.

Ничего не возвращает.
Логика: `Manager::setNoiseSuppressState`.

### `getEchoCancellerState(): String`
Получение режима эхоподавления.

Возвращает `audioprocessor`, `native` или `off`.
Логика: `Manager::getEchoCancellerState`.

### `setEchoCancellerState(state: String)`
Установка режима эхоподавления.
- `state`: `audioprocessor`, `native`, `off`, `auto`.

Ничего не возвращает.
Логика: `Manager::setEchoCancellerState`.

### `isAgcEnabled(): Boolean`
Проверка состояния AGC.

Возвращает `true`, если AGC включен.
Логика: `Manager::isAGCEnabled`.

### `setAgcState(enabled: Boolean)`
Включение/выключение AGC.
- `enabled`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setAGCState`.

### `isVADEnabled(): Boolean`
Проверка состояния VAD.

Возвращает `true`, если VAD включен.
Логика: `Manager::isVADEnabled`.

### `setVADState(enabled: Boolean)`
Включение/выключение VAD.
- `enabled`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setVADState`.

### `setAutoAnswer(accountId: String, enable: Boolean)`
Установка автоответа для аккаунта.
- `accountId`: ID учетной записи.
- `enable`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setAutoAnswer`.

### `muteDtmf(mute: Boolean)`
Включение/выключение локального воспроизведения DTMF.
- `mute`: `true` — выключить звук, `false` — включить.

Ничего не возвращает.
Логика: инвертирует `voipPreferences.playDtmf`.

### `isDtmfMuted(): Boolean`
Проверка, выключен ли звук DTMF.

Возвращает `true`, если DTMF выключен.
Логика: возвращает `not playDtmf`.

### `isCaptureMuted(): Boolean`
Проверка состояния mute для захвата.

Возвращает `true`, если захват отключён.
Логика: `AudioLayer::isCaptureMuted`.

### `muteCapture(mute: Boolean)`
Включение/выключение захвата.
- `mute`: `true`/`false`.

Ничего не возвращает.
Логика: `AudioLayer::muteCapture`.

### `isPlaybackMuted(): Boolean`
Проверка mute воспроизведения.

Возвращает `true`, если воспроизведение отключено.
Логика: `AudioLayer::isPlaybackMuted`.

### `mutePlayback(mute: Boolean)`
Включение/выключение воспроизведения.
- `mute`: `true`/`false`.

Ничего не возвращает.
Логика: `AudioLayer::mutePlayback`.

### `isRingtoneMuted(): Boolean`
Проверка mute рингтона.

Возвращает `true`, если рингтон отключён.
Логика: `AudioLayer::isRingtoneMuted`.

### `muteRingtone(mute: Boolean)`
Включение/выключение рингтона.
- `mute`: `true`/`false`.

Ничего не возвращает.
Логика: `AudioLayer::muteRingtone`.

### `getRingtoneEnabled(accountId: String): Boolean`
Проверка включён ли рингтон для аккаунта.
- `accountId`: ID учетной записи.

Возвращает `true`, если рингтон включён.
Логика: `Manager::getRingtoneEnabled`.

### `setRingtoneEnabled(accountId: String, enabled: Boolean)`
Включение/выключение рингтона для аккаунта.
- `accountId`: ID учетной записи.
- `enabled`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setRingtoneEnabled`.

### `setRingtone(accountId: String, ringtone: String): Boolean`
Установка пути к рингтону для аккаунта.
- `accountId`: ID учетной записи.
- `ringtone`: путь к файлу.

Возвращает `true`, если путь установлен.
Логика: `Manager::setRingtone`.

### `getRingtonePath(accountId: String): String`
Получение пути к рингтону аккаунта.
- `accountId`: ID учетной записи.

Возвращает путь к файлу.
Логика: `Manager::getRingtonePath`.

### `setTsxTimers(t1: UInt, t2: UInt, t4: UInt, td: UInt)`
Установка SIP-таймеров транзакций.
- `t1`, `t2`, `t4`, `td`: значения таймеров.

Ничего не возвращает.
Логика: проксирует в `Manager::setTsxTimers`.

### `getSupportedAudioManagers(): List<String>`
Получение списка поддерживаемых аудио-менеджеров.

Возвращает список строк.
Логика: `AudioPreference::getSupportedAudioManagers`.

### `getAudioManager(): String`
Получение текущего аудио-менеджера.

Возвращает строку идентификатора.
Логика: `Manager::getAudioManager`.

### `setAudioManager(api: String): Boolean`
Выбор аудио-менеджера.
- `api`: идентификатор аудио-менеджера.

Возвращает `true`, если переключение успешно.
Логика: `Manager::setAudioManager`.

### `isInitialized(): Boolean`
Проверка инициализации ядра.

Возвращает `true`, если `Manager::initialized`.
Логика: читает глобальный флаг.

### `getRecordPath(): String`
Получение пути для записей.

Возвращает строку пути.
Логика: `audioPreference.getRecordPath`.

### `setRecordPath(recPath: String)`
Установка пути для записей.
- `recPath`: путь.

Ничего не возвращает.
Логика: `audioPreference.setRecordPath`.

### `getHomePath(): String`
Получение домашнего пути пользователя.

Возвращает строку пути.
Логика: `Manager::getHomePath`.

### `getIsAlwaysRecording(): Boolean`
Проверка режима постоянной записи.

Возвращает `true`, если режим включён.
Логика: `Manager::getIsAlwaysRecording`.

### `setIsAlwaysRecording(rec: Boolean)`
Установка режима постоянной записи.
- `rec`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setIsAlwaysRecording`.

### `getRecordPreview(): Boolean`
Проверка предпросмотра записи.

Возвращает `true`, если включён предпросмотр (только при `ENABLE_VIDEO`).
Логика: читает `videoPreferences.getRecordPreview`.

### `setRecordPreview(rec: Boolean)`
Включение/выключение предпросмотра записи.
- `rec`: `true`/`false`.

Ничего не возвращает.
Логика: обновляет `videoPreferences` и сохраняет конфиг (если `ENABLE_VIDEO`).

### `getRecordQuality(): Int`
Получение качества записи.

Возвращает целочисленное качество (0 если видео отключено).
Логика: читает `videoPreferences.getRecordQuality`.

### `setRecordQuality(quality: Int)`
Установка качества записи.
- `quality`: целочисленное качество.

Ничего не возвращает.
Логика: обновляет `videoPreferences` и сохраняет конфиг.

### `setHistoryLimit(days: Int)`
Ограничение истории звонков.
- `days`: количество дней.

Ничего не возвращает.
Логика: `Manager::setHistoryLimit`.

### `getHistoryLimit(): Int`
Получение текущего ограничения истории.

Возвращает количество дней.
Логика: `Manager::getHistoryLimit`.

### `setRingingTimeout(timeout: Int)`
Установка таймаута звонка.
- `timeout`: значение в секундах.

Ничего не возвращает.
Логика: `Manager::setRingingTimeout`.

### `getRingingTimeout(): Int`
Получение таймаута звонка.

Возвращает значение в секундах.
Логика: `Manager::getRingingTimeout`.

### `setAccountsOrder(order: String)`
Установка порядка аккаунтов.
- `order`: строка с ID в нужном порядке (формат зависит от клиента).

Ничего не возвращает.
Логика: `Manager::setAccountsOrder`.

### `getCredentials(accountID: String): List<Map<String, String>>`
Получение списка креденшелов аккаунта.
- `accountID`: ID учетной записи.

Возвращает список словарей с ключами `realm`, `username`, `password`, `hash`.
Логика: возвращает `sipaccount->getCredentials()`.

### `setCredentials(accountID: String, details: List<Map<String, String>>)`
Установка списка креденшелов.
- `accountID`: ID учетной записи.
- `details`: список словарей с ключами `realm`, `username`, `password`, `hash`.

Ничего не возвращает.
Логика: выполняет unregister, обновляет конфиг, перерегистрирует и сохраняет настройки.

### `getAddrFromInterfaceName(iface: String): String`
Получение IPv4 адреса по имени интерфейса.
- `iface`: имя интерфейса.

Возвращает IP-адрес строкой.
Логика: `ip_utils::getInterfaceAddr`.

### `getAllIpInterface(): List<String>`
Получение списка IP-адресов интерфейсов.

Возвращает список адресов.
Логика: `ip_utils::getAllIpInterface`.

### `getAllIpInterfaceByName(): List<String>`
Получение списка имен интерфейсов.

Возвращает список имен.
Логика: `ip_utils::getAllIpInterfaceByName`.

### `setVolume(device: String, value: Int)`
Установка громкости.
- `device`: `speaker` или `mic`.
- `value`: значение 0–100.

Ничего не возвращает.
Логика: конвертирует в 0.0–1.0 и вызывает `setPlaybackGain`/`setCaptureGain`.

### `getVolume(device: String): Int`
Получение громкости.
- `device`: `speaker` или `mic`.

Возвращает значение 0–100.
Логика: читает gain и масштабирует.

### `connectivityChanged()`
Уведомление о смене сетевой связности.

Ничего не возвращает.
Логика: вызывает `connectivityChanged()` для всех аккаунтов.

### `setPushNotificationToken(pushDeviceToken: String)`
Установка токена push-уведомлений.
- `pushDeviceToken`: токен (пустая строка — выключить).

Ничего не возвращает.
Логика: `Manager::setPushNotificationToken`.

### `setPushNotificationTopic(topic: String)`
Установка топика push-уведомлений.
- `topic`: iOS bundle_id или bundle_id.voip.

Ничего не возвращает.
Логика: `Manager::setPushNotificationTopic`.

### `pushNotificationReceived(from: String, data: Map<String, String>)`
Передача данных полученного push-уведомления.
- `from`: отправитель.
- `data`: словарь payload.

Ничего не возвращает.
Логика: `Manager::pushNotificationReceived`.

### `isAudioMeterActive(id: String): Boolean`
Проверка активности аудио-метра.
- `id`: ID ring buffer (пустая строка — любой активный).

Возвращает `true`, если измерение активно.
Логика: `RingBufferPool::isAudioMeterActive`.

### `setAudioMeterState(id: String, state: Boolean)`
Включение/выключение аудио-метра.
- `id`: ID ring buffer (пустая строка — для всех).
- `state`: `true`/`false`.

Ничего не возвращает.
Логика: `RingBufferPool::setAudioMeterState`.

### `setDefaultModerator(accountID: String, peerURI: String, state: Boolean)`
Установка модератора по умолчанию.
- `accountID`: ID учетной записи.
- `peerURI`: URI участника.
- `state`: `true` — добавить, `false` — удалить.

Ничего не возвращает.
Логика: `Manager::setDefaultModerator`.

### `getDefaultModerators(accountID: String): List<String>`
Получение списка модераторов по умолчанию.
- `accountID`: ID учетной записи.

Возвращает список URI.
Логика: `Manager::getDefaultModerators`.

### `enableLocalModerators(accountID: String, isModEnabled: Boolean)`
Включение/выключение локальных модераторов.
- `accountID`: ID учетной записи.
- `isModEnabled`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::enableLocalModerators`.

### `isLocalModeratorsEnabled(accountID: String): Boolean`
Проверка флага локальных модераторов.
- `accountID`: ID учетной записи.

Возвращает `true`, если включено.
Логика: `Manager::isLocalModeratorsEnabled`.

### `setAllModerators(accountID: String, allModerators: Boolean)`
Сделать всех модераторами.
- `accountID`: ID учетной записи.
- `allModerators`: `true`/`false`.

Ничего не возвращает.
Логика: `Manager::setAllModerators`.

### `isAllModerators(accountID: String): Boolean`
Проверка режима «все модераторы».
- `accountID`: ID учетной записи.

Возвращает `true`, если режим включён.
Логика: `Manager::isAllModerators`.

# PresenceController - операции с подписками:

### `registerPresHandlers(handlers: Map<String, Callback>)`
Регистрация обработчиков сигналов присутствия.
- `handlers`: словарь `имя_сигнала -> CallbackWrapperBase`.

Ничего не возвращает.
Логика: проксирует в `registerSignalHandlers`.

### `publish(accountID: String, status: Boolean, note: String)`
Публикация собственного присутствия.
- `accountID`: ID учетной записи.
- `status`: `true` — online, `false` — offline.
- `note`: текстовый комментарий.

Ничего не возвращает.
Логика: если presence включён и поддерживает publish — вызывает `pres->sendPresence`.

### `answerServerRequest(uri: String, flag: Boolean)`
Ответ на запрос подписки от сервера (устаревший IP2IP путь).
- `uri`: URI запроса.
- `flag`: `true` — принять, `false` — отклонить.

Ничего не возвращает.
Логика: в текущей версии метод ничего не делает и логирует предупреждение.

> **IMPORTANT (WARNING)**: IP2IP поддержка удалена, метод не выполняет действий.

### `subscribeBuddy(accountID: String, uri: String, flag: Boolean)`
Подписка/отписка на присутствие контакта.
- `accountID`: ID учетной записи.
- `uri`: SIP URI контакта.
- `flag`: `true` — подписаться, `false` — отписаться.

Ничего не возвращает.
Логика: если presence включён и поддерживает subscribe — вызывает `pres->subscribeClient`.

### `subscribeToEvents(accountID: String, uri: String, eventType: String, flag: Boolean)`
Подписка/отписка на произвольные SIP события.
- `accountID`: ID учетной записи.
- `uri`: URI ресурса.
- `eventType`: тип события.
- `flag`: `true` — подписаться, `false` — отписаться.

Ничего не возвращает.
Логика: вызывает `SIPEvents::subscribeClient`.

### `getSubscriptions(accountID: String): List<Map<String, String>>`
Получение списка активных подписок.
- `accountID`: ID учетной записи.

Возвращает список словарей:
- `Buddy`: URI контакта;
- `Status`: `Online`/`Offline`;
- `LineStatus`: строка статуса линии.

Логика: собирает список из `Presence::getClientSubscriptions`.

### `setSubscriptions(accountID: String, uris: List<String>)`
Пакетная подписка на список URI.
- `accountID`: ID учетной записи.
- `uris`: список SIP URI.

Ничего не возвращает.
Логика: для каждого URI вызывает `pres->subscribeClient(u, true)`.

# VideoController - операции с видео:

### `registerVideoHandlers(handlers: Map<String, Callback>)`
Регистрация обработчиков видео-сигналов.
- `handlers`: словарь `имя_сигнала -> CallbackWrapperBase`.

Ничего не возвращает.
Логика: проксирует в `registerSignalHandlers`.

### `getDeviceList(): List<String>`
Список доступных видео-устройств.

Возвращает список ID устройств.
Логика: `videoDeviceMonitor.getDeviceList()`.

### `getCapabilities(deviceId: String): VideoCapabilities`
Получение возможностей устройства.
- `deviceId`: ID устройства.

Возвращает структуру `VideoCapabilities` (форматы/разрешения/частоты).
Логика: `videoDeviceMonitor.getCapabilities(deviceId)`.

### `getSettings(deviceId: String): Map<String, String>`
Получение текущих настроек устройства.
- `deviceId`: ID устройства.

Возвращает словарь (например `unique_id`, `name`, `channel`, `video_size`, `framerate`).
Логика: `videoDeviceMonitor.getSettings(deviceId).to_map()`.

### `applySettings(deviceId: String, settings: Map<String, String>)`
Применение настроек устройства.
- `deviceId`: ID устройства.
- `settings`: словарь с параметрами (`channel`, `video_size`, `framerate`).

Ничего не возвращает.
Логика: применяет настройки и сохраняет конфиг.

### `setDefaultDevice(deviceId: String)`
Установка устройства по умолчанию.
- `deviceId`: ID устройства.

Ничего не возвращает.
Логика: устанавливает дефолтный девайс и сохраняет конфиг.

### `setDeviceOrientation(deviceId: String, angle: Int)`
Установка ориентации устройства.
- `deviceId`: ID устройства.
- `angle`: угол в градусах.

Ничего не возвращает.
Логика: проксирует в `VideoManager::setDeviceOrientation`.

### `getDeviceParams(deviceId: String): Map<String, String>`
Получение параметров устройства.
- `deviceId`: ID устройства.

Возвращает словарь `format`, `width`, `height`, `rate`.
Логика: `videoDeviceMonitor.getDeviceParams()`.

### `getDefaultDevice(): String`
Получение ID устройства по умолчанию.

Возвращает ID устройства.
Логика: `videoDeviceMonitor.getDefaultDevice()`.

### `startAudioDevice()`
Запуск аудио-превью.

Ничего не возвращает.
Логика: открывает аудио-вход для превью и сбрасывает ввод.

### `stopAudioDevice()`
Остановка аудио-превью.

Ничего не возвращает.
Логика: освобождает аудио-превью.

### `openVideoInput(path: String): String`
Открытие видео-входа.
- `path`: MRL или пустая строка (тогда используется дефолтное устройство).

Возвращает ID видео-входа (MRL).
Логика: создаёт/кеширует `VideoInput` по MRL.

### `closeVideoInput(id: String): Boolean`
Закрытие видео-входа.
- `id`: ID видео-входа.

Возвращает `true`, если вход удалён.
Логика: удаляет `clientVideoInputs[id]`.

### `createMediaPlayer(path: String): String`
Создание медиаплеера.
- `path`: путь к файлу.

Возвращает ID плеера.
Логика: `createMediaPlayer`.

### `closeMediaPlayer(id: String): Boolean`
Закрытие медиаплеера.
- `id`: ID плеера.

Возвращает `true`, если закрыт.
Логика: `closeMediaPlayer`.

### `pausePlayer(id: String, pause: Boolean): Boolean`
Пауза/возобновление плеера.
- `id`: ID плеера.
- `pause`: `true` — пауза, `false` — продолжить.

Возвращает `true` при успехе.
Логика: `pausePlayer`.

### `mutePlayerAudio(id: String, mute: Boolean): Boolean`
Отключение аудио плеера.
- `id`: ID плеера.
- `mute`: `true`/`false`.

Возвращает `true` при успехе.
Логика: `mutePlayerAudio`.

### `playerSeekToTime(id: String, time: Int): Boolean`
Перемотка плеера.
- `id`: ID плеера.
- `time`: позиция в секундах.

Возвращает `true` при успехе.
Логика: `playerSeekToTime`.

### `registerSinkTarget(sinkId: String, target: SinkTarget): Boolean`
Регистрация целевого получателя кадров.
- `sinkId`: ID sink.
- `target`: структура с `pull`, `push`, `preferredFormat`.

Возвращает `true`, если sink найден.
Логика: регистрирует целевой sink в `SinkClient`.

### `startShmSink(sinkId: String, value: Boolean)`
Включение/выключение SHM sink (если `ENABLE_SHM`).
- `sinkId`: ID sink.
- `value`: `true`/`false`.

Ничего не возвращает.
Логика: `sink->enableShm` при наличии sink.

### `getRenderer(callId: String): Map<String, String>`
Получение информации о видео-рендерере.
- `callId`: ID вызова.

Возвращает словарь `CALL_ID`, `SHM_PATH`, `WIDTH`, `HEIGHT`.
Логика: если sink найден — заполняет фактические значения, иначе возвращает пустые/нулевые.

### `startLocalMediaRecorder(videoInputId: String, filepath: String): String`
Запуск локальной записи видеовхода.
- `videoInputId`: ID видеовхода.
- `filepath`: путь к файлу (расширение может быть добавлено автоматически).

Возвращает итоговый путь к файлу или пустую строку при ошибке.
Логика: создаёт `LocalRecorder`, добавляет в менеджер и запускает запись.

### `stopLocalRecorder(filepath: String)`
Остановка локальной записи.
- `filepath`: путь к файлу записи.

Ничего не возвращает.
Логика: останавливает запись и удаляет рекордер.

### `addVideoDevice(node: String, devInfo: List<Map<String, String>> = [])`
Добавление видео-устройства (Android/iOS/UWP).
- `node`: идентификатор устройства.
- `devInfo`: метаданные устройства.

Ничего не возвращает.
Логика: `videoDeviceMonitor.addDevice`.

### `removeVideoDevice(node: String)`
Удаление видео-устройства (Android/iOS/UWP).
- `node`: идентификатор устройства.

Ничего не возвращает.
Логика: `videoDeviceMonitor.removeDevice`.

### `getNewFrame(id: String): VideoFrame*`
Получение нового кадра (Android/iOS).
- `id`: ID видеовхода.

Возвращает указатель на `VideoFrame` или `nullptr`.
Логика: делегирует в `VideoManager::getVideoInput` и выделение буфера.

### `publishFrame(id: String)`
Публикация кадра в ядро.
- `id`: ID видеовхода.

Ничего не возвращает.
Логика: находит `VideoInput` и публикует кадр.

### `setVideoFrame(jenv: JNIEnv*, frame: jbyteArray, frame_size: Int, target: Long, w: Int, h: Int, rotation: Int)`
Передача сырых данных кадра (Android).
- `jenv`: JNI окружение.
- `frame`: массив байт.
- `frame_size`: размер массива.
- `target`: идентификатор окна.
- `w`, `h`: размер кадра.
- `rotation`: угол поворота.

Ничего не возвращает.
Логика: копирует данные в native window с учётом поворота.

### `acquireNativeWindow(jenv: JNIEnv*, javaSurface: jobject): Long`
Получение `ANativeWindow` из Java Surface (Android).
- `jenv`: JNI окружение.
- `javaSurface`: объект Surface.

Возвращает указатель (Long) на `ANativeWindow`.
Логика: `ANativeWindow_fromSurface`.

### `releaseNativeWindow(windowId: Long)`
Освобождение `ANativeWindow` (Android).
- `windowId`: идентификатор окна.

Ничего не возвращает.
Логика: `ANativeWindow_release`.

### `captureVideoFrame(javaVM: JavaVM*, jenv: JNIEnv*, inputId: String, javaImage: jobject, rotation: Int)`
Передача кадра из Android `Image` в SIP core.
- `javaVM`: JVM.
- `jenv`: JNI окружение.
- `inputId`: ID видеовхода.
- `javaImage`: объект `android.media.Image`.
- `rotation`: угол поворота.

Ничего не возвращает.
Логика: формирует `VideoFrame` и публикует его.

### `captureVideoPacket(inputId: String, data: Pointer, size: Int, offset: Int, keyframe: Boolean, timestamp: Long, rotation: Int)`
Передача закодированного видео-пакета (Android).
- `inputId`: ID видеовхода.
- `data`: указатель на буфер.
- `size`: размер пакета.
- `offset`: смещение в буфере.
- `keyframe`: признак ключевого кадра.
- `timestamp`: временная метка.
- `rotation`: угол поворота.

Ничего не возвращает.
Логика: упаковывает данные в `AVPacket` и публикует кадр.

### `setNativeWindowGeometry(windowId: Long, width: Int, height: Int)`
Установка размеров native window (Android).
- `windowId`: идентификатор окна.
- `width`, `height`: размеры.

Ничего не возвращает.
Логика: `ANativeWindow_setBuffersGeometry`.

### `registerVideoCallback(sink: String, windowId: Long): Boolean`
Привязка sink к native window (Android).
- `sink`: ID sink.
- `windowId`: идентификатор окна.

Возвращает `true`, если callback зарегистрирован.
Логика: создаёт `SinkTarget` с pull/push callbacks и регистрирует.

### `unregisterVideoCallback(sink: String, windowId: Long)`
Отвязка sink от native window (Android).
- `sink`: ID sink.
- `windowId`: идентификатор окна.

Ничего не возвращает.
Логика: очищает sink target и удаляет окно из таблицы.

### `getDecodingAccelerated(): Boolean`
Проверка аппаратного ускорения декодирования.

Возвращает `true`, если ускорение доступно (при `RING_ACCEL`).
Логика: читает `videoPreferences.getDecodingAccelerated`.

### `setDecodingAccelerated(state: Boolean)`
Включение/выключение аппаратного декодирования.
- `state`: `true`/`false`.

Ничего не возвращает.
Логика: обновляет настройки и сохраняет конфиг (при `RING_ACCEL`).

### `getEncodingAccelerated(): Boolean`
Проверка аппаратного ускорения кодирования.

Возвращает `true`, если ускорение доступно (при `RING_ACCEL`).
Логика: читает `videoPreferences.getEncodingAccelerated`.

### `setEncodingAccelerated(state: Boolean)`
Включение/выключение аппаратного кодирования.
- `state`: `true`/`false`.

Ничего не возвращает.
Логика: обновляет настройки, сохраняет конфиг и обновляет активность HEVC кодека.

# CallNotifier - события, относящиеся к определенным звонкам:

### `callStateChanged(accountId: String, callId: String, state: String, detailCode: Int)`
Метод, уведомляющий клиента об изменении состояния вызова.
- `accountId`: ID учетной записи для идентификации пользователя.
- `callId`: ID вызова, состояние которого изменилось.
- `state`: Новое состояние вызова. Возможные значения:
    - "INCOMING": Входящий вызов. Играет рингтон
    - "CONNECTING": Устанавливается соединение с собеседником.
    - "RINGING": Новый исходщий вызов, играют гудки.
    - "CURRENT": Текущий активный вызов, идет разговор
    - "HUNGUP": Вызов завершен.
    - "BUSY": Мы отклонили входящий вызов.
    - "PEER_BUSY": Собеседник занят и отклонил вызов
    - "FAILURE": Вызов завершен с ошибкой.
    - "HOLD": Вызов приостановлен.
    - "UNHOLD": Вызов возобновлен.
    - "INACTIVE": Вызов неактивен.
    - "OVER": Вызов завершен, все потоки закрыты.
- `detailCode`: последний полученный SIP-код.

### `transferStateChange(accountId: String, callId: String, subscriptionState: String, lastCode: Int, message: String)`
Метод, уведомляющий клиента о статусе подписки на перевод (REFER).
- `accountId`: ID учетной записи для идентификации пользователя.
- `callId`: ID переводимого вызова
- `subscriptionState`: Статус подписки ('active' / 'terminated' / 'error' / 'pending').
- `lastCode`: Последний SIP код, полученный при REFER подписке от удаленной стороны. Здесь можно получить код ошибки, если статус == 'error'. Если перевод успешен, то здесь будет 200.
- 'message': Последнее сообщение, полученное от удаленной стороне при REFER подписке. Например, во время выполнения перевода это будет Trying, если при переводе возникла ошибка, то здесь будет причина, по которой подписка завершилась неудачно.

### `recordPlaybackStopped(path: String)`
Метод, уведомляющий клиента о завершении воспроизведения записанного файла.
- `path`: Путь к файлу, который был воспроизведен.

### `incomingMessage(accountId: String, callId: String, from: String, messages: Map<String, String>)`
Метод, уведомляющий клиента о входящем сообщении.
- `accountId`: ID учетной записи для идентификации пользователя.
- `callId`: ID вызова, в котором было получено сообщение.
- `from`: Адрес отправителя сообщения.
- `messages`: Сообщение, которое было получено.

### `incomingCallWithMedia(accountId: String, callId: String, from: String, mediaList: List<Map<String, String>>)`
Метод для уведомления клиента о входящем вызове с медиа.
- `accountId`: ID учетной записи для идентификации пользователя.
- `callId`: ID входящего вызова.
- `from`: Адрес отправителя вызова.
- `mediaList`: Список медиа-потоков.
Каждый элемент должен содержать:
    - "MEDIA_TYPE" - тип медиапотока: "MEDIA_TYPE_AUDIO" или "MEDIA_TYPE_VIDEO";
    - "ENABLED" - флаг, включено ли медиа (отображать в SDP): "true" или "false";
    - "MUTED" - флаг, включен ли поток медиа: "true" или "false";
    - "LABEL" - для дебага, например my_super_audio.

### `mediaChangeRequested(accountId: String, callId: String, mediaList: List<Map<String, String>>)`
Метод для уведомления клиента о запросе на изменение медиа-ресурсов.
- `accountId`: ID учетной записи для идентификации пользователя.
- `callId`: ID вызова, для которого запрошено изменение медиа-ресурсов.
- `mediaList`: Список медиа-потоков.
Каждый элемент должен содержать:
    - "MEDIA_TYPE" - тип медиапотока: "MEDIA_TYPE_AUDIO" или "MEDIA_TYPE_VIDEO";
    - "ENABLED" - флаг, включено ли медиа (отображать в SDP): "true" или "false";
    - "MUTED" - флаг, включен ли поток медиа: "true" или "false";
    - "LABEL" - для дебага, например my_super_audio.

### `recordPlaybackFilepath(id: String, filename: String)`
Метод для уведомления клиента о пути записанного файла.
- `id`: ID записанного файла.
- `filename`: Путь к записанному файлу.

### `conferenceCreated(accountId: String, confId: String)`
Метод для уведомления клиента о создании конференции.
- `accountId`: ID учетной записи для идентификации пользователя.
- `confId`: ID созданной конференции.

### `conferenceChanged(accountId: String, confId: String, state: String)`
Метод для уведомления клиента о изменении состояния конференции.
- `accountId`: ID учетной записи для идентификации пользователя.
- `confId`: ID конференции, состояние которой изменилось.
- `state`: Новое состояние конференции.

### `conferenceRemoved(accountId: String, confId: String)`
Метод для уведомления клиента о удалении конференции.
- `accountId`: ID учетной записи для идентификации пользователя.
- `confId`: ID конференции, которую нужно удалить.

### `recordingStateChanged(callId: String, code: Int)`
Метод для уведомления клиента об изменении состояния записи вызова.
- `callId`: ID вызова, состояние записи которого изменилось.
- `code`: Код состояния записи.

### `onConferenceInfosUpdated(confId: String, infos: List<Map<String, String>>)`
Метод для уведомления клиента об обновлении информации о конференции.
- `confId`: ID конференции.
- `infos`: Список с информацией о конференции.

### `audioMuted(callId: String, muted: Boolean)`
Метод для уведомления клиента о выключении/включении звука в вызове.
- `callId`: ID вызова.
- `muted`: Флаг, указывающий, выключен ли звук в вызове.

### `videoMuted(callId: String, muted: Boolean)`
Метод для уведомления клиента о выключении/включении видео в вызове.
- `callId`: ID вызова.
- `muted`: Флаг, указывающий, выключено ли видео в вызове.

### `connectionUpdate(id: String, state: Int)`
Метод для уведомления клиента об изменении состояния соединения.
- `id`: ID соединения.
- `state`: Состояние соединения.

### `remoteRecordingChanged(callId: String, peerNumber: String, state: Boolean)`
Метод для уведомления клиента об изменении состояния записи вызова.
- `callId`: ID вызова.
- `peerNumber`: Номер пира.
- `state`: Состояние записи вызова.

### `mediaNegotiationStatus(callId: String, event: String, mediaList: List<Map<String, String>>)`
Метод для уведомления клиента о состоянии процесса медиа-согласования в вызове.
- `callId`: ID вызова.
- `event`: Событие, связанное с медиа-согласованием.
- `mediaList`: Список с информацией о медиа.
Каждый элемент должен содержать:
    - "MEDIA_TYPE" - тип медиапотока: "MEDIA_TYPE_AUDIO" или "MEDIA_TYPE_VIDEO";
    - "ENABLED" - флаг, включено ли медиа (отображать в SDP): "true" или "false";
    - "MUTED" - флаг, включен ли поток медиа: "true" или "false";
    - "LABEL" - для дебага, например my_super_audio.

# ConfigurationNotifier - события, относящиеся к аккаунтам и конфигурации приложения:

### `volumeChanged(device: String, value: Int)`
Метод для уведомления клиента о том, что изменилась громкость устройства.
- `device`: Имя устройства, громкость которого изменилась.
- `value`: Новое значение громкости.

### `accountsChanged()`
Метод для уведомления клиента о том, что изменились учетные записи.

### `accountDetailsChanged(accountId: String, details: Map<String, String>)`
Метод для уведомления клиента о том, что изменились детали учетной записи.
- `accountId`: ID учетной записи, детали которой изменились.
- `details`: Измененные детали учетной записи.

### `registrationStateChanged(accountId: String, state: String, code: Int, detail_str: String)`
Метод для уведомления клиента о том, что изменилось состояние регистрации учетной записи.
- `accountId`: ID учетной записи, состояние регистрации которой изменилось.
- `state`: Новое состояние регистрации. Возможные значения:
    - "REGISTERED": Учетная запись зарегистрирована успешно.
    - "READY": Учетная запись готова к использованию.
    - "UNREGISTERED": Учетная запись была разрегетрирована успешно.
    - "TRYING": Происходит попытка зарегистрировать учетную запись.
    - "ERROR_GENERIC": Общая ошибка в процессе регистрации.
    - "ERROR_AUTH": Ошибка аутентификации при регистрации учетной записи.
    - "ERROR_NETWORK": Ошибка сети в процессе регистрации.
    - "ERROR_HOST": Ошибка при попытке подключения к серверу.
- `code`: Код состояния регистрации.
- `detail_str`: Подробности о состоянии регистрации.

### `incomingAccountMessage(accountId: String, from: String, message_id: String, payload: Map<String, String>)`
Метод для уведомления клиента о входящем сообщении от учетной записи.
- `accountId`: ID учетной записи, которая получила сообщение.
- `from`: Адрес отправителя сообщения.
- `message_id`: ID сообщения.
- `payload`: Карта, содержащая полезную нагрузку сообщения.

### `accountMessageStatusChanged(accountId: String, conversationId: String, peer: String, message_id: String, state: Int)`
Метод для уведомления клиента об изменении статуса сообщения учетной записи.
- `accountId`: ID учетной записи, которой принадлежит сообщение.
- `conversationId`: ID диалога, к которому относится сообщение.
- `peer`: Адрес получателя сообщения.
- `message_id`: ID сообщения.
- `state`: Новое состояние сообщения.

### `activeCallsChanged(accountId: String, conversationId: String,  activeCalls: List<Map<String, String>>)`
Метод для уведомления клиента об изменении списка активных вызовов.
- `accountId`: ID учетной записи, для которой произошли изменения.
- `conversationId`: ID диалога, связанного с изменениями.
- `activeCalls`: Список словарей, описывающих активные вызовы.

### `hardwareDecodingChanged(state: Boolean)`
Метод для уведомления клиента о том, что произошли изменения в аппаратном декодировании.
- `state`: Флаг, указывающий, включено ли аппаратное декодирование.

### `hardwareEncodingChanged(state: Boolean)`
Метод для уведомления клиента о том, что произошли изменения в аппаратном кодировании.
- `state`: Флаг, указывающий, включено ли аппаратное кодирование.

### `audioMeter(id: Int, level: Float)`
Метод для уведомления клиента о уровне звука устройства.
- `id`: Идентификатор устройства.
- `level`: Уровень звука.

# PresenceNotifier - события, относящиеся к подпискам:
### `subscribtionError(accountId: String, error: String, msg: String)`
Метод для уведомления клиента о ошибке, возникшей при подписке
- `accountId`: ID учетной записи, которая инициировала подписку.
- `error`: Тип ошибки.
- `msg`: Сообщение об ошибке.

### `newBuddyNotification(accountId: String, buddyUri: String, lineStatus: Boolean, note: String)`
Метод для уведомления клиента о новом статусе контакта.
- `accountId`: ID учетной записи, связанной с контактом.
- `buddyUri`: URI контакта **sip:username@domain**.
- `lineStatus`: Доступность линии контакта.
- `note`: Комментарий к статусу пользователя.

### `subscriptionStateChanged(accountId: String, buddyUri: String, active: Boolean)`
Метод для уведомления клиента об изменении состояния подписки.
- `accountId`: ID учетной записи, связанной с подпиской.
- `buddyUri`: URI контакта, состояние подписки которого изменилось **sip:username@domain**.
- `active`: Новое состояние подписки.

# VideoNotifier - события, относящиеся к видео:

### `startCapture(camid: String)`
Уведомление о том что начался захват с устройства
- `camid`: ID камеры

### `stopCapture(camid: String)`
Уведомление о том что захват с устройства закончился
- `camid`: ID камеры

### `decodingStarted(id: String, shmPath: String, w: Int, h: Int, ixMixer: Boolean)`
Метод, уведомляющий клиента о начале декодирования видео.
- `id`: ID вызова, связанного с декодированием.
- `shmPath`: Путь к разделяемой памяти, содержащей декодированные данные (**LINUX ONLY**).
- `w`: Ширина видео.
- `h`: Высота видео.
- `ixMixer`: Флаг, указывающий, является ли декодирование частью микшера (те является ли этот декодер - отображением получившейся конференции).

### `decodingStopped(id: String, shmPath: String, ixMixer: Boolean)`
Метод, уведомляющий клиента о завершении декодирования видео.
- `id`: ID вызова, связанного с декодированием.
- `shmPath`: Путь к разделяемой памяти, содержащей декодированные данные (**LINUX ONLY**).
- `ixMixer`: Флаг, указывающий, является ли декодирование частью микшера.

### `deviceAdded(camId: String)`
Метод, уведомляющий клиента о добавлении нового устройства.
- `camId`: ID добавленного устройства.
- `off`: шумоподавление отключено
