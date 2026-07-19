# Dola

Dola — небольшой статически типизированный язык программирования с нативной
компиляцией. Он подходит для консольных программ и сетевых сервисов; в
репозитории также находится многопользовательский чат, целиком написанный на
Dola.

## Возможности языка

- модули, импорты, публичные и закрытые объявления;
- функции и типы `Unit`, `Bool`, `Int`, `Float` и `String`;
- неизменяемые привязки `let` и изменяемые переменные `var`;
- записи, перечисления и кортежи;
- `Option[T]`, `Result[T, E]` и оператор `?`;
- исчерпывающий `match` с сопоставлением с образцом;
- неизменяемые списки и словари;
- циклы `loop`, `while` и `for`, а также `break` и `continue`;
- задачи `spawn`, типизированные каналы и ожидание результата задачи;
- типизированные TCP-соединения с фреймингом и сериализацией MessagePack;
- сложение строк и преобразование `Int.to_string()`;
- компиляция в нативный исполняемый файл через MLIR и LLVM.

Пользовательские обобщённые типы, трейты, замыкания, макросы, `async`/`await`
и менеджер пакетов в язык не входят.

## Поддерживаемые платформы

- macOS на Apple Silicon (`arm64`);
- Linux на `x86_64`.

Сборка выполняется для текущей платформы. Кросс-компиляция не поддерживается.
При первом запуске нужен доступ к сети для загрузки зависимостей Bazel; затем
они берутся из локального кеша.

## Установка Bazel через Bazelisk

[Bazelisk](https://github.com/bazelbuild/bazelisk) автоматически выбирает
версию Bazel из файла `.bazelversion` и при необходимости загружает её.

На macOS установите Bazelisk через Homebrew:

```bash
brew install bazelisk
```

На Linux `x86_64` можно установить официальный бинарный файл в пользовательский
каталог:

```bash
mkdir -p "$HOME/.local/bin"
curl -fLo "$HOME/.local/bin/bazel" \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x "$HOME/.local/bin/bazel"
export PATH="$HOME/.local/bin:$PATH"
```

Альтернативный способ для macOS и Linux при наличии Go:

```bash
go install github.com/bazelbuild/bazelisk@latest
export PATH="$(go env GOPATH)/bin:$PATH"
bazelisk version
```

В последнем случае команды ниже можно выполнять как через `bazelisk`, так и
через символическую ссылку с именем `bazel`. Проверить установку:

```bash
bazel version
```

## Сборка и тесты

Все команды выполняются из корня каталога `dola/`:

```bash
bazel build //...
bazel test //...
```

Проверки форматирования и статического анализа запускаются отдельно:

```bash
bazel test //:format_check
bazel test //:lint
bazel test //:quality
```

Автоматически отформатировать C++ и Rust:

```bash
bazel run //:format
```

Cargo, CMake, системный ANTLR и установленные вручную LLVM/MLIR не требуются.

Полная проверка на Linux `x86_64`, используемая CI:

```bash
./ci/install_bazelisk.sh "$HOME/.local/bin/bazel"
export PATH="$HOME/.local/bin:$PATH"
./ci/run_linux_x86_64.sh
```

## Компилятор

Справка по командной строке:

```bash
bazel run //tools/dola -- --help
```

Доступные режимы:

```text
--dump-ast           вывести AST
--check              проверить программу
--emit-mlir          вывести типизированный MLIR
--emit-lowered-mlir  вывести MLIR после понижения
--emit-llvm          вывести LLVM IR
--emit-object        создать объектный файл; путь задаётся через -o
```

Например:

```bash
bazel run //tools/dola -- \
  --check examples/hello/hello.dola

bazel run //tools/dola -- \
  --emit-llvm examples/arithmetic/arithmetic.dola

bazel run //tools/dola -- \
  --emit-object -o /tmp/hello.o examples/hello/hello.dola
```

Если программа состоит из нескольких модулей, передайте компилятору все её
исходные файлы.

## Правила Bazel для Dola

Подключение правил:

```python
load("//bazel:dola_rules.bzl", "dola_binary", "dola_library", "dola_test")
```

Библиотека распространяет модули на зависимые цели:

```python
dola_library(
    name = "protocol",
    srcs = ["protocol.dola"],
)
```

Нативная программа компилирует собственные и транзитивные исходники одной
командой:

```python
dola_binary(
    name = "server",
    srcs = ["server.dola"],
    deps = [":protocol"],
)
```

Исполняемый тест задаётся аналогично:

```python
dola_test(
    name = "example_test",
    srcs = ["example_test.dola"],
)
```

Импортируемый модуль обязательно должен находиться в `srcs` или транзитивных
`deps`; поиск незаявленных файлов в рабочем каталоге не выполняется.

## Примеры

```bash
bazel run //examples/hello
bazel run //examples/arithmetic
bazel run //examples/records
bazel run //examples/collections
bazel run //examples/concurrency
bazel run //examples/transport
```

## Чат

Запустите сервер в первом терминале:

```bash
bazel run //examples/chat:server
```

Сервер напечатает адрес после `CHAT_LISTENING`, например
`127.0.0.1:49152`. Во втором терминале запустите клиент:

```bash
bazel run //examples/chat:client
```

Клиент ожидает две первые строки ввода: адрес сервера и уникальное имя
пользователя.

```text
127.0.0.1:49152
Alice
```

Обычная строка отправляет публичное сообщение. Доступные команды:

```text
/history N       показать последние N публичных сообщений
/users           показать подключённых пользователей
/msg NAME TEXT   отправить личное сообщение
/quit            выйти
```

Имена чувствительны к регистру, занимают от 1 до 32 байт UTF-8 и не содержат
ASCII-пробелов. Публичное или личное сообщение может занимать не более 4096
байт UTF-8. Сервер хранит последние 100 публичных сообщений.
