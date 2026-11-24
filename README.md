# stdio.sys

Собираемый MVP для Windows 11 x64, начиная с 24H2, с основным путём для локальных томов NTFS. Код написан на C17. Пользовательские программы используют статический CRT MSVC и системные API Windows.

## Компоненты

| Файл | Назначение |
| --- | --- |
| `stdio.exe` | Запускаемый по требованию процесс для команд Проводника. Выполняет операцию от имени текущего пользователя и завершает работу. |
| `stdio.dll` | COM-расширение `IExplorerCommand`. Добавляет в меню папки команды подсчёта размера и удаления без корзины с подтверждением. |
| `stdioctl.exe` | Команды файловых операций, поиска, диагностики и бенчмарков. |
| `stdio.sys`, `stdio.inf` | Необязательный лабораторный минифильтр с защищённым диагностическим портом. Загружается по требованию, экземпляр присоединяется вручную. |

Операции движка выполняются в user mode. У минифильтра нет обработчиков файлового ввода-вывода. Эффект от переноса операций в ядро предстоит подтвердить отдельным экспериментом. Постоянная служба SCM в MVP отсутствует. Интерактивный индекс живёт на время сессии и ждёт ввода без активного опроса.

## Сборка

В x64 Native Tools Command Prompt для Visual Studio с MSVC, Windows SDK 10.0.26100.0 или новее, CMake 3.24+ и Ninja:

```bat
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\release
ctest --test-dir build\release --output-on-failure
cmake --install build\release --prefix build\dist
```

Для драйвера нужны заголовки и библиотеки WDK x64. Можно использовать установленный WDK либо каталог `c` распакованного официального пакета `Microsoft.Windows.WDK.x64` версии `10.0.26100.1`. Каталог должен содержать `Include\10.0.26100.0\km` и `Lib\10.0.26100.0\km\x64`. [Microsoft WDK NuGet](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget)

```bat
driver\build.cmd "C:\Program Files (x86)\Windows Kits\10" 10.0.26100.0
InfVerif /u driver\stdio.inf
Inf2Cat /driver:"%CD%\build\driver" /os:10_GE_X64 /uselocaltime
```

`Inf2Cat` создаёт каталог `build\driver\stdio.cat`. Подпись пакета выполняется отдельно. [Сборка каталога драйвера](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/inf2cat)

## Проводник и CLI

Размести три пользовательских файла в одной постоянной папке. Установи расширение из обычной командной строки текущего пользователя. Команды рассчитаны на одну выбранную папку и доступны в классическом контекстном меню **Показать дополнительные параметры**.

```bat
%SystemRoot%\System32\regsvr32.exe "%CD%\build\dist\stdio.dll"
```

Обычные команды Delete, Ctrl+C, Ctrl+X, Ctrl+V и поиск Проводника выполняет Windows. Для современного верхнего уровня меню Microsoft документирует `IExplorerCommand` с идентичностью пакета. В MVP регистрация идёт через HKCU, без MSIX. [Интеграция с контекстным меню Windows 11](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer)

```bat
stdioctl size "D:\Data\Project"
stdioctl list "D:\Data\Project"
stdioctl find "D:\Data\Project" report
stdioctl delete "D:\Data\Generated" --yes auto
stdioctl recycle "D:\Data\Trash me"
stdioctl copy "D:\Backup" "D:\Data\Project"
stdioctl copy-new "D:\Backup\New replica" "D:\Data\Project" auto
stdioctl copy-new "E:\Project" "D:\Data\Project" auto
stdioctl move "D:\Archive" "D:\Data\Project"
stdioctl move-new "D:\Archive\Project" "D:\Data\Project"
stdioctl copy-file "D:\Data\source.txt" "D:\Backup\new.txt"
stdioctl copy-file "D:\Data\archive.bin" "E:\archive.bin"
stdioctl storage-info "D:\Data"
stdioctl index-dir "D:\Data\Project"
stdioctl index D:
```

- `size` суммирует логические размеры основных потоков по именам. Жёсткие ссылки учитываются как отдельные записи каталога. Размеры ADS и выделенных кластеров в сумму не входят. Каталоги перечисляются блоками, а `list` сортирует записи по имени, используя общий буфер имён.
- `delete` обрабатывает дерево снизу вверх, открывает элементы относительно дескрипторов каталогов и сверяет ID и тип объекта перед удалением. Вложенная junction удаляется как запись. Путь к выбранному корню через reparse point и удаление корня диска отклоняются. Глубина обхода ограничена 256 уровнями, включая выбранный каталог. Заблокированные, недоступные и read-only объекты остаются на месте. При частичном удалении команда возвращает первый код ошибки и число обработанных элементов. Повторный запуск работает с остатком дерева.
- Автовыбор использует 1 поток для HDD или неизвестного устройства, до 2 для SSD и до 4 для NVMe с учётом доступных логических процессоров. Мелкие пачки обрабатываются последовательно. Для сравнений и диагностики можно явно задать 1–16 потоков. Адаптация к текущей фоновой нагрузке ещё не реализована.
- `recycle`, `copy` и `move` вызывают штатный `IFileOperation`, который обрабатывает конфликты и отмену. Для корзины используются флаги Windows Shell, включая предупреждение системы, если объект будет уничтожен. `copy-file` вызывает `CopyFile2` с запретом перезаписи и сохранением символических ссылок. ADS и атрибуты копируются, ACL нового файла наследуется от назначения. [Флаги IFileOperation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags) · [Семантика CopyFile2](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfile2)
- `copy-new` создаёт новое дерево и распределяет независимые вызовы `CopyFile2` по ограниченному пулу потоков. Для источника и назначения предусмотрены локальные NTFS, exFAT и FAT32, включая съёмные тома. Выбирается меньший предел из лимитов обоих носителей. Времена и обычные атрибуты каталогов сохраняются с точностью файловой системы назначения. FAT32 допускает файлы до 4 ГиБ минус один байт и не поддерживает ADS и расширенные атрибуты. Если том назначения не поддерживает эти метаданные, команда отклоняет соответствующий файл. [Сравнение файловых систем Windows](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison/)
- Встреча с reparse point, offline-объектом, сжатым или зашифрованным каталогом завершает `copy-new` с ошибкой. На NTFS новые объекты наследуют права назначения. Для каталогов с дополнительными потоками, ссылками или специальными метаданными используй `copy`. При ошибке уже завершённые файлы остаются в новом дереве.
- Соседние ветви могут копироваться параллельно. Когда одна ветвь заканчивается, её поток может помогать с файлами в другой ветви. Один общий лимит действует на всю операцию. При единственной ветви потоки работают с независимыми файлами внутри неё.
- По умолчанию Windows копирует с буферизацией. Для одного большого видео или архива `copy-file` передаёт файл целиком в `CopyFile2`. Пул движка используется для независимых файлов. Для сравнения на конкретном носителе `copy-file --unbuffered` включает режим без системного кэша, а `copy-new --unbuffered-large` включает его для обычных файлов от 512 МиБ. Проверенные локальные прогоны проводились на NVMe, проверка на физической флешке предстоит. [Параметры CopyFile2](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-copyfile2_extended_parameters)
- `storage-info` показывает файловую систему, поддержку клонирования блоков томом и результат запроса аппаратного копирования ODX. Клонирование блоков применимо в пределах одного поддерживаемого тома. [Возможности файловой системы](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw) · [Запрос возможностей ODX](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-device_copy_offload_descriptor)
- `move-new` переименовывает объект в пределах тома одним вызовом Win32 с запретом замены существующего назначения. Межтомовое перемещение выполняет команда `move` через Windows Shell. [Семантика MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- При неподдерживаемом пути команда размера в Проводнике может открыть обычные свойства папки. Частично выполненное удаление требует явного повторного запуска с выбранной пользователем семантикой.

### Поиск и кэш

`index-dir PATH` работает с явно выбранным деревом под текущими правами. Он хранит имена файлов общими блоками. При поиске по имени маска трёхсимвольных последовательностей быстро исключает неподходящие ASCII-имена, а оставшиеся кандидаты сопоставляет Windows. Уведомление Windows инвалидирует кэш, а следующий запрос перестраивает снимок каталога. Результаты поиска проверяются по идентификатору файла и текущим атрибутам через обычное открытие Windows. Команда `folder-size` использует последний снимок. Снимок отражает состояние файлов, которые Windows уже успела включить в уведомления, в том числе после закрытия или сброса буфера записи.

`index X:` использует `FSCTL_ENUM_USN_DATA` для первичного построения индекса MFT и `FSCTL_READ_USN_JOURNAL` для обновления перед запросом. При сбросе или усечении журнала индекс перестраивается. Для режима нужен доступ к тому с правами администратора и активный USN Journal. Каждая запись MFT хранит одно имя на файл, поэтому этот режим может пропустить другие имена жёстких ссылок. Поиск по размеру читает актуальные метаданные кандидатов, и время растёт с их количеством. Индексы хранятся в памяти процесса. [USN Journal](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-operations)

```text
name report
ext txt
path Project\2026
size 1024:1048576
count name report
folder-size
quit
```

`folder-size` доступна в режиме `index-dir`. `count` печатает количество совпадений и время запроса без вывода списка. Для разового поиска без построения индекса доступна команда `find`.

Когда `count` набирает большую пачку кандидатов, проверку файлов распределяют до четырёх потоков с учётом типа накопителя. Идентификатор и логический размер читаются одним запросом на открытом дескрипторе NTFS, а при отсутствии поддержки используется штатный запрос Win32. [Классы NtQueryInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/content/ntifs/nf-ntifs-ntqueryinformationfile)

Удаление расширения из профиля текущего пользователя:

```bat
%SystemRoot%\System32\regsvr32.exe /u "%CD%\build\dist\stdio.dll"
```

После выгрузки DLL Проводником можно удалить три пользовательских файла. Если файл занят, выйди из учётной записи и войди снова.

## Лабораторный драйвер

Первую загрузку выполняй в Windows 11 x64 VM со снимком и отдельным тестовым томом. Для загрузки нужен пакет, подписанный по правилам политики подписи гостевой Windows. В сборке есть неподписанный каталог. Проверка Secure Boot требует подписи, которой доверяет активная политика Secure Boot. Высота в INF предназначена для лабораторных проверок. Для публичного распространения требуется высота по правилам Microsoft. [Подпись драйверов](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing) · [Тестирование с Secure Boot](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/preproduction-driver-signing-and-install) · [Выделение высоты](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/allocated-altitudes) · [Проверка HVCI](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/driver-compatibility-with-device-guard)

В гостевой системе из командной строки администратора:

```bat
pnputil /add-driver "C:\Lab\stdio\stdio.inf" /install
stdioctl driver-load
stdioctl driver-status
fltmc attach stdio E:
fltmc instances -f stdio
fltmc detach stdio E:
stdioctl driver-unload
pnputil /enum-drivers /class ActivityMonitor
pnputil /delete-driver oemNN.inf /uninstall
```

Сверь опубликованное имя `oemNN.inf` с исходным именем `stdio.inf` и поставщиком `stdio.sys`. Для повторных прогонов проверь перезапуск, выгрузку с открытым диагностическим соединением, ручное присоединение к NTFS и отказ для других файловых систем. Driver Verifier включается для `stdio.sys` только в VM, с отдельным прогоном проверки Code Integrity. [Примитивные драйверы Windows](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/creating-a-primitive-driver) · [PnPUtil](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)

## Производительность

[Методика и результаты](docs/benchmarks.md)
