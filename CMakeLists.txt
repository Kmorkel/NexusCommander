cmake_minimum_required(VERSION 3.18)
project(NexusCommander VERSION 1.1.0 LANGUAGES CXX)

# NexusCommander uses Win32 APIs throughout (drives, network shares, taskbar)
if(NOT WIN32)
    message(FATAL_ERROR "NexusCommander builds on Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── MSVC: Locate Qt automatically ────────────────────────────────────────────
# Visual Studio has no Qt environment of its own, so search C:\Qt for the
# newest MSVC build of Qt, then fall back to QT_ROOT / QTDIR.
# MinGW is built from Qt Creator, which passes the kit's compiler and Qt.
if(MSVC AND NOT CMAKE_PREFIX_PATH)
    # Only version folders (C:/Qt/6.x.y/msvc*), never C:/Qt/Tools
    file(GLOB _qt_dirs LIST_DIRECTORIES true "C:/Qt/[0-9]*/msvc*")
    # NATURAL compare, newest first: 6.12.0 > 6.9.1 (string sort gets this wrong)
    list(SORT _qt_dirs COMPARE NATURAL ORDER DESCENDING)

    # Newest 64-bit build, else the newest build of any architecture
    set(_qt_found "")
    foreach(_dir IN LISTS _qt_dirs)
        if(_dir MATCHES "msvc[0-9]+_64$")
            set(_qt_found "${_dir}")
            break()
        endif()
    endforeach()
    if(NOT _qt_found AND _qt_dirs)
        list(GET _qt_dirs 0 _qt_found)
    endif()

    if(_qt_found)
        set(CMAKE_PREFIX_PATH "${_qt_found}" CACHE PATH "Auto-detected Qt path")
        message(STATUS "Auto-detected Qt at: ${_qt_found}")
    elseif(DEFINED ENV{QT_ROOT})
        set(CMAKE_PREFIX_PATH "$ENV{QT_ROOT}" CACHE PATH "Qt path from QT_ROOT")
        message(STATUS "Using Qt from QT_ROOT: $ENV{QT_ROOT}")
    elseif(DEFINED ENV{QTDIR})
        set(CMAKE_PREFIX_PATH "$ENV{QTDIR}" CACHE PATH "Qt path from QTDIR")
        message(STATUS "Using Qt from QTDIR: $ENV{QTDIR}")
    else()
        message(FATAL_ERROR
            "Qt installation not found!\n"
            "Searched:\n"
            "  1. C:\\Qt\\<version>\\msvc*        (no match)\n"
            "  2. Environment variable QT_ROOT  (not set)\n"
            "  3. Environment variable QTDIR    (not set)\n"
            "\n"
            "Please do ONE of the following:\n"
            "  a) Install Qt with the MSVC component into C:\\Qt\\\n"
            "  b) Set the environment variable QT_ROOT, e.g.:\n"
            "       set QT_ROOT=D:\\Qt\\6.12.0\\msvc2022_64\n"
            "  c) Pass the path directly on the command line:\n"
            "       cmake -B build -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64\n")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network)
# Private API: FilePanel::addServerNode() seeds QFileSystemModel's server node
# (qfilesystemmodel_p.h) — Qt would stat the server on the GUI thread
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)

# ── Executable ───────────────────────────────────────────────────────────────
qt_add_executable(NexusCommander WIN32
    main.cpp
    mainwindow.h            mainwindow.cpp          mainwindow.ui
    filePanel.h             filePanel.cpp
    connectServerDialog.h   connectServerDialog.cpp
    fileSystemUtils.h
    fileJob.h
    fileWorkerThread.h      fileWorkerThread.cpp
    jobModel.h              jobModel.cpp
    jobQueueWidget.h        jobQueueWidget.cpp
    clipboardUtils.h        clipboardUtils.cpp
    driveScanner.h          driveScanner.cpp
    networkDiscovery.h      networkDiscovery.cpp
    folderSizeCache.h       folderSizeCache.cpp
    taskbarProgress.h       taskbarProgress.cpp
    resources/resources.qrc
    resources/app.rc        # embeds nexuscommander.ico as the .exe icon
)

# windres (MinGW) does not search the .rc file's own folder for the .ico
set_source_files_properties(resources/app.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/resources")

# Single source of the app version: project(VERSION) → APP_VERSION
# (main.cpp sets it as applicationVersion, the About dialog reads that)
target_compile_definitions(NexusCommander PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(NexusCommander PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::GuiPrivate
    mpr        # WNet* API: mapped network drives      (driveScanner.cpp)
    netapi32    # NetShareEnum: share listing           (networkDiscovery.cpp)
    credui      # Windows sign-in dialog                (networkDiscovery.cpp)
    ole32 uuid  # ITaskbarList3: taskbar progress       (taskbarProgress.cpp)
)

# Visual Studio: NexusCommander as startup project instead of ALL_BUILD
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT NexusCommander)

# ── Deployment: windeployqt as a post-build step ─────────────────────────────
# Prefer the windeployqt of the Qt that find_package() actually found
find_program(WINDEPLOYQT windeployqt
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
if(NOT WINDEPLOYQT)
    message(WARNING "windeployqt not found – Qt DLLs must be copied manually")

elseif(MSVC)
    # MSVC: every configuration, Qt DLLs next to the .exe so it runs and debugs
    # straight from Visual Studio / Qt Creator. Debug gets the debug DLLs
    # (Qt6Cored.dll …) – a debug build cannot run with the release DLLs.
    add_custom_command(TARGET NexusCommander POST_BUILD
        COMMAND "${WINDEPLOYQT}"
                $<IF:$<CONFIG:Debug>,--debug,--release>
                "$<TARGET_FILE:NexusCommander>"
        COMMENT "Deploying Qt DLLs next to $<TARGET_FILE_NAME:NexusCommander>..."
        VERBATIM)

elseif(MINGW AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # MinGW: every configuration except Debug, self-contained copy in deploy/
    # – zip it up and ship it. No Debug deploy: Qt's MinGW packages ship no
    # debug plugins, and Qt Creator sets PATH itself when debugging.
    set(DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy")

    # windeployqt finds the MinGW runtime only when g++ is on PATH, so copy
    # it explicitly from the compiler's folder to make deploy/ always complete.
    # (The rest of the C runtime, UCRT, is part of Windows 10 / 11.)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_mingw_runtime "")
    foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${_mingw_bin}/${_dll}")
            list(APPEND _mingw_runtime "${_mingw_bin}/${_dll}")
        else()
            message(WARNING "MinGW runtime ${_dll} not found in ${_mingw_bin}")
        endif()
    endforeach()
    set(_copy_runtime "")
    if(_mingw_runtime)
        set(_copy_runtime COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                                  ${_mingw_runtime} "${DEPLOY_DIR}")
    endif()

    add_custom_command(TARGET NexusCommander POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:NexusCommander>" "${DEPLOY_DIR}"
        COMMAND "${WINDEPLOYQT}" --release --dir "${DEPLOY_DIR}"
                "${DEPLOY_DIR}/$<TARGET_FILE_NAME:NexusCommander>"
        ${_copy_runtime}
        COMMENT "Deploying Qt and MinGW runtime DLLs to ${DEPLOY_DIR}..."
        VERBATIM)
endif()
