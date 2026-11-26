# Phase 0 单元测试（Qt Test）
# 输出目录固定到 <build>/tests，避免测试进程在应用目录旁创建 app_data 污染正式配置。

qt6_add_resources(CALENDAR_RESOURCES ${PROJECT_SOURCE_DIR}/calendar.qrc)

set(PHASE0_CORE_SOURCES
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/matterstore.cpp ${PROJECT_SOURCE_DIR}/src/memory/privatereflection.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actionorigin.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.h
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
    ${PROJECT_SOURCE_DIR}/src/app/sessionrouter.h
    ${PROJECT_SOURCE_DIR}/src/app/sessionrouter.cpp
)

# ctest 直接运行 exe 时需能找到 Qt 与 MinGW 运行库
get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_test_path "${_qt_bin_dir};${_mingw_bin_dir}")
# ENVIRONMENT 是分号分隔的列表，PATH 值内的分号需转义
string(REPLACE ";" "\\;" _test_path_escaped "${_test_path}")

function(add_phase0_test name)
    qt_add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${MAKO_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        ENVIRONMENT "PATH=${_test_path_escaped}"
    )
endfunction()

add_phase0_test(tst_runtimelog tst_runtimelog.cpp ${PROJECT_SOURCE_DIR}/src/app/runtimelog.cpp)

add_phase0_test(tst_memorymanager
    tst_memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/app/privatereview.cpp
    ${PROJECT_SOURCE_DIR}/src/app/privatereview.h
    ${PROJECT_SOURCE_DIR}/src/benchmark/benchmarksnapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrieval.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrieval.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryembedding.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryembedding.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorydashscope.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorydashscope.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryindexworker.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryindexworker.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/matterstore.cpp ${PROJECT_SOURCE_DIR}/src/memory/privatereflection.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actionorigin.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_memorymanager PRIVATE Qt6::Network)

add_phase0_test(tst_configmanager
    tst_configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.h
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
)

add_phase0_test(tst_memorypolicy
    tst_memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)

add_phase0_test(tst_recentcontext
    tst_recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
)

add_phase0_test(tst_memorytextutils
    tst_memorytextutils.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytextutils.h
)

add_phase0_test(tst_memoryaccesspolicy
    tst_memoryaccesspolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryaccesspolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryaccesspolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/historyturn.h
)

add_phase0_test(tst_groupmembership
    tst_groupmembership.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/groupmembershipservice.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/groupmembershipservice.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqprotocolclient.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqprotocolclient.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/matterstore.cpp ${PROJECT_SOURCE_DIR}/src/memory/privatereflection.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.h
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
)
target_link_libraries(tst_groupmembership PRIVATE Qt6::WebSockets Qt6::Network)

add_phase0_test(tst_memoryagent
    ${PROJECT_SOURCE_DIR}/src/app/privatereview.cpp
    ${PROJECT_SOURCE_DIR}/src/app/cognitivecoordinator.cpp
    tst_memoryagent.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/imageperception.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/cognitivesnapshot.h
    ${PROJECT_SOURCE_DIR}/src/cognition/cognitivesnapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/llmservice.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryoutputguard.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/promptassembler.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrieval.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrievalworker.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryembedding.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorydashscope.cpp
    ${CALENDAR_RESOURCES}
    ${PROJECT_SOURCE_DIR}/src/memory/memoryagenttypes.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryagent.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryagent.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryresultjson.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryresultjson.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltool.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltool.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltypes.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltypes.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeframe.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeframe.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/festivalcalendar.h
    ${PROJECT_SOURCE_DIR}/src/memory/festivalcalendar.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/timeframeresolver.h
    ${PROJECT_SOURCE_DIR}/src/memory/timeframeresolver.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorysourcepolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorysourcepolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryoverview.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryoverview.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeresolver.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeresolver.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryexecutor.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryexecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/matterstore.cpp ${PROJECT_SOURCE_DIR}/src/memory/privatereflection.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryaccesspolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryaccesspolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimerange.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimerange.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytextutils.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_memoryagent PRIVATE Qt6::Network Qt6::Gui)
set_target_properties(tst_memoryagent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/memory-agent")

add_phase0_test(tst_interactionsegmenter
    tst_interactionsegmenter.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/interactionsegmenter.h
    ${PROJECT_SOURCE_DIR}/src/memory/interactionsegmenter.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)

add_phase0_test(tst_memoryoutputguard
    tst_memoryoutputguard.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryoutputguard.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryoutputguard.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)

add_phase0_test(tst_sessionrouter
    tst_sessionrouter.cpp
    ${PHASE0_CORE_SOURCES}
)

add_phase0_test(tst_promptassembler
    tst_promptassembler.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/promptassembler.h
    ${PROJECT_SOURCE_DIR}/src/cognition/promptassembler.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltypes.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryrecalltypes.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeframe.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorytimeframe.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryresultjson.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryresultjson.cpp
)

add_phase0_test(tst_qqreplypolicy
    tst_qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
)

add_phase0_test(tst_qqmessageparser
    tst_qqmessageparser.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)

add_phase0_test(tst_globalmoodstate
    tst_globalmoodstate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/globalmoodstate.h
    ${PROJECT_SOURCE_DIR}/src/ui/globalmoodstate.cpp
)
add_phase0_test(tst_ttslifecycle
    tst_ttslifecycle.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/ttsservice.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/ttsprocessmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/mockttsprovider.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/apittsprovider.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/ttsreferencemanager.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/ittsprovider.h
    ${PROJECT_SOURCE_DIR}/src/audio/iaudiosink.h
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_ttslifecycle PRIVATE Qt6::Network)

add_phase0_test(tst_qqdelivery
    tst_qqdelivery.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqchanneladapter.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqprotocolclient.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_qqdelivery PRIVATE Qt6::Network Qt6::WebSockets)

add_phase0_test(tst_settingspages
    tst_settingspages.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/settingspages.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/ttsreferencemanager.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_settingspages PRIVATE Qt6::Widgets)
set_target_properties(tst_settingspages PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/settings-ui")
set_property(TEST tst_settingspages APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 关注（concern）能力回归：直接复用 tst_memorymanager 的整套源文件，
# 只把测试入口换成 tst_concern.cpp，并与 tst_memorymanager 一样链接 Qt6::Network。
add_phase0_test(tst_concern
    tst_concern.cpp
    ${PROJECT_SOURCE_DIR}/src/app/privatereview.cpp
    ${PROJECT_SOURCE_DIR}/src/app/privatereview.h
    ${PROJECT_SOURCE_DIR}/src/benchmark/benchmarksnapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/config/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryevidence.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrieval.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryretrieval.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryembedding.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryembedding.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memorydashscope.h
    ${PROJECT_SOURCE_DIR}/src/memory/memorydashscope.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/memoryindexworker.h
    ${PROJECT_SOURCE_DIR}/src/memory/memoryindexworker.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/matterstore.cpp ${PROJECT_SOURCE_DIR}/src/memory/privatereflection.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actionpolicy.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.cpp
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.h
    ${PROJECT_SOURCE_DIR}/src/memory/recentcontext.cpp
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/src/channels/qq/qqmessageparser.cpp
)
target_link_libraries(tst_concern PRIVATE Qt6::Network)

# 认知快照（报告切片 A）：服务端事实分层 + authority 分类 + 提示词 JSON 预算。
# 复用 tst_sessionrouter 的 MemoryManager 最小依赖集，另加 cognitivesnapshot.cpp。
add_phase0_test(tst_cognitivesnapshot
    tst_cognitivesnapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/cognitivesnapshot.h
    ${PROJECT_SOURCE_DIR}/src/cognition/cognitivesnapshot.cpp
    ${PHASE0_CORE_SOURCES}
)

# 主动外发统一提交/Outbox（报告切片 B）：域状态 CAS + cognitive_action + action_outbox
# 同一事务提交，OutboxDispatcher 唯一发送，回执/恢复的状态机回归。
# 复用 tst_sessionrouter 的 MemoryManager 最小依赖集，另加 actioncommitter/actionoutboxdispatcher。
add_phase0_test(tst_actionoutbox
    tst_actionoutbox.cpp
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.h
    ${PROJECT_SOURCE_DIR}/src/cognition/actioncommitter.cpp
    ${PROJECT_SOURCE_DIR}/src/app/actionoutboxdispatcher.h
    ${PROJECT_SOURCE_DIR}/src/app/actionoutboxdispatcher.cpp
    ${PHASE0_CORE_SOURCES}
)

# 主动联系策略（报告切片 C）：动机/事实分离、确定性 claim 校验 + 可注入语义验证器。
# 用临时 MemoryManager 造真实数据，验证器用假函数注入，覆盖允许/压下/修复/降级路径。
add_phase0_test(tst_actionpolicy
    tst_actionpolicy.cpp
    ${PHASE0_CORE_SOURCES}
)
