#!/bin/bash

# Checks if directory exists, otherwise asks to install package.
check_dir_exists() {
    local path=$1
    local package=$2

    if [ ! -d "$path" ]; then
        echo "Please install $package"
        exit 1
    fi
}

if [ -z "$BARRIER_BUILD_ENV" ]; then
    if ! xcode-select --print-path > /dev/null 2>&1; then
        echo "Please install the Xcode command line tools ('xcode-select --install') or full Xcode"
        exit 1
    fi

    # works under both the Command Line Tools and full Xcode layouts
    BARRIER_SDK_PATH=$(xcrun --show-sdk-path 2>/dev/null)
    check_dir_exists "$BARRIER_SDK_PATH" "the macOS SDK ('xcode-select --install' or full Xcode)"
    export BARRIER_SDK_PATH

    printf "Modifying environment for Barrier build...\n"

    if command -v port; then
        printf "Detected Macports\n"

        check_dir_exists '/opt/local/lib/cmake/Qt5' 'qt5-qtbase port'

        export BARRIER_BUILD_MACPORTS=1
        export CMAKE_PREFIX_PATH="/opt/local/lib/cmake/Qt5:$CMAKE_PREFIX_PATH"
        export DYLD_LIBRARY_PATH="/opt/local/lib:$DYLD_LIBRARY_PATH"
        export CPATH="/opt/local/include:$CPATH"
        export PKG_CONFIG_PATH="/opt/local/libexec/qt5/lib/pkgconfig:$PKG_CONFIG_PATH"

    elif command -v brew; then
        printf "Detected Homebrew\n"
        QT_PATH=$(brew --prefix qt@5)
        BREW_PREFIX=$(brew --prefix)

        check_dir_exists "$QT_PATH" 'qt5'

        export BARRIER_BUILD_BREW=1
        export CMAKE_PREFIX_PATH="$QT_PATH:$BREW_PREFIX:$CMAKE_PREFIX_PATH"
        export DYLD_LIBRARY_PATH="$BREW_PREFIX/lib:$DYLD_LIBRARY_PATH"
        export CPATH="$BREW_PREFIX/include:$CPATH"
        export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

        if [ -d /opt/procursus ]; then
            export CMAKE_PREFIX_PATH="/opt/procursus:$CMAKE_PREFIX_PATH"
            export DYLD_LIBRARY_PATH="/opt/procursus/lib:$DYLD_LIBRARY_PATH"
            export CPATH="/opt/procursus/include:$CPATH"
            export PKG_CONFIG_PATH="/opt/procursus/lib/pkgconfig:$PKG_CONFIG_PATH"
        fi
    else
        printf "Neither Homebrew nor Macports is installed. Can't get dependency paths\n"
        exit 1
    fi

    export BARRIER_BUILD_ENV=1

    printf "done\n"
fi
