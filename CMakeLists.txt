cmake_minimum_required(VERSION 3.19)
project(Windows_AI LANGUAGES CXX)
enable_language(RC)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql REQUIRED Multimedia)

qt_standard_project_setup()

# 添加资源文件
qt6_add_resources(RESOURCE_FILES image.qrc)

qt_add_executable(Windows_AI
    WIN32 MACOSX_BUNDLE
    main.cpp
    characterwidget.cpp
    characterwidget.h
    ${RESOURCE_FILES}
    bubblewidget.h bubblewidget.cpp bubblewidget.ui
    configmanager.h configmanager.cpp
    llmservice.h llmservice.cpp
    appcontroller.h appcontroller.cpp
    sentencedata.h
    appearancemanager.h appearancemanager.cpp
    ttsservice.h ttsservice.cpp
    chatwidget.h chatwidget.cpp
    anchorstrategy.h
    anchormanager.h anchormanager.cpp
    memorymanager.h memorymanager.cpp
    historyturn.h
    utterance.h
    ichanneladapter.h
    petchanneladapter.h
    petchanneladapter.cpp
    qqchanneladapter.h
    qqchanneladapter.cpp
    sessionrouter.h
    sessionrouter.cpp
    ttsreference.h
    ittsprovider.h
    apittsprovider.h apittsprovider.cpp
    mockttsprovider.h mockttsprovider.cpp
    ttsreferencemanager.h ttsreferencemanager.cpp
    ipcmplayer.h ipcmplayer.cpp
    streamplayer.h streamplayer.cpp
    fileplayer.h fileplayer.cpp
    ttsprocessmanager.h ttsprocessmanager.cpp
    timemanager.h timemanager.cpp
    app.rc
    settingswidget.h settingswidget.cpp settingswidget.ui
)

target_link_libraries(Windows_AI
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::Sql
        Qt::Multimedia
)

install(TARGETS Windows_AI
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET Windows_AI
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
target_include_directories(Windows_AI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ===== 自动化测试（Qt Test，ctest 运行：ctest --test-dir <build> -C Debug --output-on-failure）=====
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()