# Сборка

Сборка производится в два этапа.

1. **Этап конфигурирования** -  на этом этапе идет не только генерирование скриптов сборки, но и сборка всех зависимостей. Обычно именно на этом возникают ошибки. Зависимости появляются в папке **contrib/triplet**, где triplet - это описание целевой платформы в компиляторах GCC и Clang. Триплет состоит из трех частей: архитектуры, поставщика и операционной системы. Например, в случае **`x86_64-redhat-linux`**:
    - `x86_64`: архитектура процессора (64-битная версия архитектуры x86).
    - `redhat`: поставщик операционной системы (в данном случае, Red Hat).
    - `linux`: операционная система (Linux)
      
    Такие триплеты используются для указания, для какой платформы предназначен компилируемый код, и помогают компилятору выбрать правильные настройки и библиотеки. Разделение собираемых зависимостей по таким папкам удобно, так как часто сборка идет на несколько разных архитектур. Например, на darwin идет сборка x86 + arm64, поэтому будет создано сразу две папки: **`arm64-apple-darwin24.0.0`** + **`x86_64-apple-darwin24.0.0`**.

    Правила для сборки для windows описываются в package.json, для linux + darwin в rules.mak. Например, для ffmpeg:
    - windows: _contrib/src/ffmpeg/package.json_
    - linux + darwin: _contrib/src/ffmpeg/rules.mak_

2. **Этап сборки** - сборка, собственно, самого ядра. На этом этапе важно быть уверенным, что линковка идет именно к библиотекам, собранным на предыдущем этапе, а не к системным, например. Это важно, так как требуется специфичные (в некоторых случаях пропатченные) версии зависимостей. В CMakeLists.txt это настраивается путем модификации переменных CMake `CMAKE_PREFIX_PATH`+ `CMAKE_FIND_ROOT_PATH`.

Вот как выглядят команды для сборки на Linux + Darwin:

```bash
mkdir build
cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/some/path -DCMAKE_BUILD_TYPE=Debug/Release
cmake --build . --target install
```

После этого библиотека должна появится в /some/path.

Для windows идет генерация сразу для Release, Debug и тд, поэтому команды немного меняются:

```bash
mkdir build
cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/some/path --A x64 -Thost=x64
cmake --build . --config Release/Debug --target install
```
# Пользовательские переменные этапа конфигурации CMake

При выполнении этапа конфигурации (генерации команд для сборки) также можно задать значения некоторых предопределённых переменных.

- `BUILD_DEPS` - собирать ли зависимости? Удобно отключать, когда зависимости уже откуда то стянуты и ждать пока они опять соберутся очень долго и проблематично.
- `ENABLE_VIDEO`- собирать ли с поддержкой видео
- `PREBUILD_DEPS_PATHS`- путь к зависимостям, если `BUILD_DEPS=OFF`. Пути должны быть структурированы в этой папке также, как в папке contrib. То есть, если сборка идет на **`x86_64-redhat-linux`**, то конечный путь к зависимостям будет выглядеть так: `${PREBUILD_DEPS_PATHS}/x86_64-redhat-linux`. Внутри этой папки зависимости должны быть разложены также, как если бы они были собраны автоматически.

# Инструменты, необходимые для сборки

## Darwin + Linux
- **gcc**
- **_make_**
- **_сmake_**
- **_yasm_**
- **_nasm_**
- **_pkg-config_**
- **_curl + wget + tar + git_**

На darwin + linux установки библиотек, содержащих данные инструменты,  любым способом - должно быть достаточно. Главное, чтобы они были доступны в PATH.

## Windows
- **_python_** (_любая_ версия)
- **_[MSYS2](https://www.msys2.org/)_**
- **_cmake_**
- **_Windows SDK + MSVC комиляторы_**
- **_[Visual Studio 2010 (VC++ 10.0) SP1](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#visual-studio-2010-vc-100-sp1-no-longer-supported)_**

Здесь все немного сложнее. Когда как на линуксе скрипты сборки зависимостей написаны на bash + make, то на windows используется Python, который вызывает для некоторых проектов сборку через MSBuild,  для некоторых через CMake, для некоторых через обычный gcc. Никакие сторонние зависимости для python устанавливать _не надо_ (через pip).

Для проектов, которые используют gcc (например ffmpeg), необходимо установить MSYS2, и затем стянуть зависимости, запустив MSYS2: 

```bash
pacman -S nasm yasm gcc make pkg-config
```

Эти зависимости должны быть доступны в PATH (python их просто вызывает напрямую, не указывая никакие абсолютные пути), поэтому необходимо прописать путь к папке bin MSYS2 в PATH. После этого сборка зависимостей которые требуют gcc должна пройти успешно - и даже если gcc создаст статические архивы с расширением .a, их можно будет использовать для линковки c помощью компоновщика MSVC, просто поменяя их расширение из .a в .lib, как это делается в _contrib/src/ffmpeg/package.json_:

```json
{
    "name": "ffmpeg",
    "version": "n5.0",
    "url": "https://nexus.svetlocal.ru/repository/github-artifacts/ffmpeg-__VERSION__.tar.gz",
    "deps": [
        "vpx",
        "x264",
        "opus"
    ],
    "patches": [
        "change-RTCP-ratio.patch",
        "rtp_ext_abs_send_time.patch",
        "rtp_dtmf.patch",
        "libopusenc-reload-packet-loss-at-encode.patch",
        "libopusdec-enable-FEC.patch",
        "windows-configure.patch"
    ],
    "win_patches": [
    ],
    "project_paths": [],
    "with_env" : "",
    "custom_scripts": {
        "pre_build": [],
        "build": [
            "call \"%CONTRIB_SRC_DIR%\\ffmpeg\\build_ffmpeg.bat\"",
            "cd \"%INSTALL_PREFIX%\\lib\" && ren *.a *.lib"
        ],
        "post_build": []
    }
}
```
Windows SDK + MSVC комиляторы очень просто устанавливаются через установщик [Visual Studio Community Edition](https://visualstudio.microsoft.com/vs/community/).

Visual Studio 2010 (VC++ 10.0) SP1 требуется для сборки некоторых зависимостей, которые используют "легаси" код. При установке Visual Studio данный пакет **не устанавливается**.