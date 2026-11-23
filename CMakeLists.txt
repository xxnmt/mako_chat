cmake_minimum_required(VERSION 3.19)
project(Windows_AI VERSION 0.3 LANGUAGES CXX)
enable_language(RC)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql Multimedia WebSockets)

qt_standard_project_setup()

# 添加资源文件
qt6_add_resources(RESOURCE_FILES image.qrc calendar.qrc)

qt_add_executable(Windows_AI
    WIN32 MACOSX_BUNDLE
    src/app/runtimelog.cpp
    src/app/main.cpp
    src/ui/characterwidget.cpp
    src/ui/characterwidget.h
    ${RESOURCE_FILES}
    src/ui/bubblewidget.h src/ui/bubblewidget.cpp src/ui/bubblewidget.ui
    src/config/configmanager.h src/config/configmanager.cpp
    src/memory/memorypolicy.h src/memory/memorypolicy.cpp src/memory/memoryreadpolicy.h
    src/memory/memoryoutputguard.h src/memory/memoryoutputguard.cpp
    src/memory/interactionsegmenter.h src/memory/interactionsegmenter.cpp
    src/memory/recentcontext.h src/memory/recentcontext.cpp
    src/memory/raweventprocessor.h src/memory/raweventprocessor.cpp
    src/channels/qq/groupmembershipservice.h src/channels/qq/groupmembershipservice.cpp
    src/memory/memoryaccesspolicy.h src/memory/memoryaccesspolicy.cpp
    src/memory/memorytimerange.h src/memory/memorytimerange.cpp
    src/memory/memoryrecalltypes.h src/memory/memoryrecalltypes.cpp
    src/memory/memorytimeframe.h src/memory/memorytimeframe.cpp
    src/memory/festivalcalendar.h src/memory/festivalcalendar.cpp
    src/memory/timeframeresolver.h src/memory/timeframeresolver.cpp
    src/memory/memorysourcepolicy.h src/memory/memorysourcepolicy.cpp
    src/memory/memoryoverview.h src/memory/memoryoverview.cpp
    src/memory/memorytimeresolver.h src/memory/memorytimeresolver.cpp
    src/memory/memoryagenttypes.h src/memory/memoryagent.h src/memory/memoryagent.cpp
    src/memory/memoryevidence.h src/memory/memoryevidence.cpp
    src/memory/memoryretrieval.h src/memory/memoryretrieval.cpp
    src/memory/memoryretrievalworker.h src/memory/memoryretrievalworker.cpp
    src/memory/memoryembedding.h src/memory/memoryembedding.cpp
    src/memory/memorydashscope.h src/memory/memorydashscope.cpp
    src/memory/memoryindexworker.h src/memory/memoryindexworker.cpp
    src/memory/memoryresultjson.h src/memory/memoryresultjson.cpp
    src/memory/memoryrecalltool.h src/memory/memoryrecalltool.cpp
    src/memory/memoryexecutor.h src/memory/memoryexecutor.cpp
    src/cognition/imageperception.cpp
    src/cognition/llmservice.h src/cognition/llmservice.cpp
    src/cognition/promptassembler.h src/cognition/promptassembler.cpp
    src/benchmark/benchmarkrunner.h src/benchmark/benchmarkrunner.cpp src/benchmark/benchmarksnapshot.h src/benchmark/benchmarksnapshot.cpp
    src/app/appcontroller.h src/app/appcontroller.cpp
    src/app/privatereview.cpp src/app/privatereview.h
    src/app/cognitivecoordinator.h src/app/cognitivecoordinator.cpp
    src/app/sentencedata.h
    src/ui/appearancemanager.h src/ui/appearancemanager.cpp
    src/ui/globalmoodstate.h src/ui/globalmoodstate.cpp
    src/audio/ttsservice.h src/audio/ttsservice.cpp
    src/ui/chatwidget.h src/ui/chatwidget.cpp
    src/ui/anchorstrategy.h
    src/ui/anchormanager.h src/ui/anchormanager.cpp
    src/memory/memorymanager.h src/memory/memorymanager.cpp src/memory/matterstore.cpp src/memory/privatereflection.cpp
    src/memory/historyturn.h
    src/app/utterance.h
    src/app/ichanneladapter.h
    src/channels/pet/petchanneladapter.h
    src/channels/pet/petchanneladapter.cpp
    src/channels/qq/qqchanneladapter.h
    src/channels/qq/qqchanneladapter.cpp
    src/app/sessionrouter.h
    src/app/sessionrouter.cpp
    src/channels/qq/qqmessageparser.h src/channels/qq/qqmessageparser.cpp
    src/channels/qq/qqreplypolicy.h src/channels/qq/qqreplypolicy.cpp
    src/channels/qq/qqprotocolclient.h src/channels/qq/qqprotocolclient.cpp
    src/audio/ttsreference.h
    src/audio/ittsprovider.h
    src/audio/apittsprovider.h src/audio/apittsprovider.cpp
    src/audio/mockttsprovider.h src/audio/mockttsprovider.cpp
    src/audio/ttsreferencemanager.h src/audio/ttsreferencemanager.cpp
    src/audio/iaudiosink.h
    src/audio/petaudiosink.h src/audio/petaudiosink.cpp
    src/audio/ipcmplayer.h src/audio/ipcmplayer.cpp
    src/audio/streamplayer.h src/audio/streamplayer.cpp
    src/audio/fileplayer.h src/audio/fileplayer.cpp
    src/audio/ttsprocessmanager.h src/audio/ttsprocessmanager.cpp
    src/ui/timemanager.h src/ui/timemanager.cpp
    app.rc
    src/ui/settingspages.h src/ui/settingspages.cpp
    src/ui/settingswidget.h src/ui/settingswidget.cpp src/ui/settingswidget.ui
)

target_link_libraries(Windows_AI
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::Sql
        Qt::Multimedia
        Qt::WebSockets
)

# 版本号由 project(VERSION) 注入，运行时日志会话头与 QCoreApplication 都会用到。
target_compile_definitions(Windows_AI PRIVATE MAKO_APP_VERSION="${PROJECT_VERSION}")

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
set(MAKO_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channels/qq
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channels/pet
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cognition
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark
)
target_include_directories(Windows_AI PRIVATE ${MAKO_INCLUDE_DIRS})

# ===== 自动化测试（Qt Test，ctest 运行：ctest --test-dir <build> -C Debug --output-on-failure）=====
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
