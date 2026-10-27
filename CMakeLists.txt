cmake_minimum_required(VERSION 3.16)
project(NexusCommander VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Windows: Automatically locate Qt installation (MSVC or MinGW) ────────────
if(WIN32 AND NOT CMAKE_PREFIX_PATH)

    # ── Step 1: Scan C:\Qt for matching installations ────────────────────────
    # Pick glob pattern based on compiler toolchain
    if(MSVC)
        file(GLOB _qt_versions "C:/Qt/*/msvc*")
        set(_qt_arch_pattern "msvc[0-9]+_64")
    elseif(MINGW)
        file(GLOB _qt_versions "C:/Qt/*/mingw*")
        set(_qt_arch_pattern "mingw[_0-9]*_64")
    else()
        file(GLOB _qt_versions "C:/Qt/*/msvc*" "C:/Qt/*/mingw*")
        set(_qt_arch_pattern "_64")
    endif()

    if(_qt_versions)
        # Sort descending so the newest Qt version wins
        list(SORT _qt_versions ORDER DESCENDING)

        # Prefer 64-bit builds on 64-bit targets
        set(_qt_found "")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            foreach(_dir IN LISTS _qt_versions)
                if(_dir MATCHES "${_qt_arch_pattern}")
                    set(_qt_found "${_dir}")
                    break()
                endif()
            endforeach()
        endif()

        # Fallback: take the first (newest) match regardless of arch
        if(NOT _qt_found)
            list(GET _qt_versions 0 _qt_found)
        endif()

        set(CMAKE_PREFIX_PATH "${_qt_found}" CACHE PATH "Auto-detected Qt path")
        message(STATUS "Auto-detected Qt at: ${_qt_found}")

    else()
        # ── Step 2: Check environment variables ──────────────────────────────
        if(DEFINED ENV{QT_ROOT})
            set(CMAKE_PREFIX_PATH "$ENV{QT_ROOT}" CACHE PATH "Qt path from QT_ROOT env")
            message(STATUS "Using Qt from QT_ROOT env: $ENV{QT_ROOT}")

        elseif(DEFINED ENV{QTDIR})
            set(CMAKE_PREFIX_PATH "$ENV{QTDIR}" CACHE PATH "Qt path from QTDIR env")
            message(STATUS "Using Qt from QTDIR env: $ENV{QTDIR}")

        else()
            # ── Step 3: Helpful error ────────────────────────────────────────
            if(MSVC)
                set(_qt_search_hint "C:\\Qt\\*\\msvc*")
                set(_qt_install_hint "Install Qt with the MSVC component into C:\\Qt\\")
                set(_qt_env_example "set QT_ROOT=D:\\Qt\\6.7.0\\msvc2022_64")
                set(_qt_cmd_example "cmake -DCMAKE_PREFIX_PATH=D:/Qt/6.7.0/msvc2022_64 ..")
            else()
                set(_qt_search_hint "C:\\Qt\\*\\mingw*")
                set(_qt_install_hint "Install Qt with the MinGW component into C:\\Qt\\")
                set(_qt_env_example "set QT_ROOT=D:\\Qt\\6.7.0\\mingw_64")
                set(_qt_cmd_example "cmake -DCMAKE_PREFIX_PATH=D:/Qt/6.7.0/mingw_64 ..")
            endif()
            message(FATAL_ERROR
                "Qt installation not found!\n"
                "Searched:\n"
                "  1. ${_qt_search_hint}  (no match)\n"
                "  2. Environment variable QT_ROOT  (not set)\n"
                "  3. Environment variable QTDIR    (not set)\n"
                "\n"
                "Please do ONE of the following:\n"
                "  a) ${_qt_install_hint}\n"
                "  b) Set the environment variable QT_ROOT, e.g.:\n"
                "       ${_qt_env_example}\n"
                "  c) Pass the path directly on the command line:\n"
                "       ${_qt_cmd_example}\n"
            )
        endif()
    endif()

endif()
# ──────────────────────────────────────────────────────────────────────────────

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Concurrent Network LinguistTools)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Concurrent Network LinguistTools)

set(TS_FILES NexusCommander_en_150.ts)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        ${TS_FILES}
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(NexusCommander
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        filePanel.h         filePanel.cpp
        connectServerDialog.h connectServerDialog.cpp
        fileSystemUtils.h
        fileJob.h
        fileWorkerThread.h  fileWorkerThread.cpp
        jobModel.h          jobModel.cpp
        jobQueueWidget.h    jobQueueWidget.cpp
        clipboardUtils.h    clipboardUtils.cpp
        driveScanner.h      driveScanner.cpp
        networkDiscovery.h  networkDiscovery.cpp
        folderSizeCache.h   folderSizeCache.cpp
        taskbarProgress.h   taskbarProgress.cpp
        resources/resources.qrc
    )
    qt_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
else()
    if(ANDROID)
        add_library(NexusCommander SHARED
            ${PROJECT_SOURCES}
        )
    else()
        add_executable(NexusCommander
            ${PROJECT_SOURCES}
        )
    endif()
    qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
endif()

target_link_libraries(NexusCommander PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::Network
)

# Single source of the app version: project(VERSION) above → APP_VERSION
# (main.cpp sets it as applicationVersion, the About dialog reads that)
target_compile_definitions(NexusCommander PRIVATE APP_VERSION="${PROJECT_VERSION}")

# Windows: mpr = WNet* API (mapped network drives, driveScanner.cpp),
#          netapi32 = NetShareEnum (share listing, networkDiscovery.cpp),
#          credui = sign-in dialog (networkDiscovery.cpp),
#          ole32 + uuid = ITaskbarList3 (taskbar progress, taskbarProgress.cpp)
if(WIN32)
    target_link_libraries(NexusCommander PRIVATE mpr netapi32 credui ole32 uuid)
endif()

# Windows: app.rc embeds nexuscommander.ico as the .exe icon (Explorer, taskbar).
# windres (MinGW) does not search the .rc file's own folder for the .ico.
if(WIN32)
    target_sources(NexusCommander PRIVATE resources/app.rc)
    set_source_files_properties(resources/app.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()

if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.NexusCommander)
endif()

set_target_properties(NexusCommander PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# ── Windows: Locate windeployqt once, deploy per toolchain ───────────────────
if(WIN32 AND (MSVC OR MINGW))
    find_program(WINDEPLOYQT windeployqt
        HINTS "${CMAKE_PREFIX_PATH}/bin"
    )
    if(NOT WINDEPLOYQT)
        message(WARNING "windeployqt not found – DLLs must be copied manually")
    endif()
endif()

# MSVC: Deploy Qt DLLs directly next to the .exe — for both Debug and Release.
# Visual Studio is a multi-config generator, so --debug/--release is selected
# via generator expression. MSVC ships proper debug Qt DLLs (Qt6Cored.dll etc.),
# so deploying inline lets us debug straight from VS without PATH workarounds.
if(WIN32 AND MSVC AND WINDEPLOYQT)
    add_custom_command(TARGET NexusCommander POST_BUILD
        COMMAND ${WINDEPLOYQT}
                $<IF:$<CONFIG:Debug>,--debug,--release>
                "$<TARGET_FILE:NexusCommander>"
        COMMENT "Deploying Qt DLLs next to $<TARGET_FILE_NAME:NexusCommander>..."
    )
endif()

# MinGW: Deploy into a separate deploy/ folder for Release only.
# Qt 6.10 MinGW ships without debug plugins (qwindowsd.dll missing), so
# `windeployqt --debug` fails. For debugging Qt Creator sets PATH itself,
# so no deploy/ folder is needed.
if(WIN32 AND MINGW AND WINDEPLOYQT AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy")
    add_custom_command(TARGET NexusCommander POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:NexusCommander> "${DEPLOY_DIR}/"
        COMMAND ${WINDEPLOYQT}
                --release
                --dir "${DEPLOY_DIR}"
                "${DEPLOY_DIR}/$<TARGET_FILE_NAME:NexusCommander>"
        COMMENT "Deploying Qt DLLs to ${DEPLOY_DIR}..."
    )
endif()

include(GNUInstallDirs)
install(TARGETS NexusCommander
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(NexusCommander)
endif()

# Visual Studio: set NexusCommander as default startup project instead of ALL_BUILD
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT NexusCommander)