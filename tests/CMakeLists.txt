# Phase 0 单元测试（Qt Test）
# 输出目录固定到 <build>/tests，避免测试进程在应用目录旁创建 app_data 污染正式配置。

set(PHASE0_CORE_SOURCES
    ${PROJECT_SOURCE_DIR}/memorymanager.h
    ${PROJECT_SOURCE_DIR}/memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/configmanager.h
    ${PROJECT_SOURCE_DIR}/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/sessionrouter.h
    ${PROJECT_SOURCE_DIR}/sessionrouter.cpp
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
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
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

add_phase0_test(tst_memorymanager
    tst_memorymanager.cpp
    ${PROJECT_SOURCE_DIR}/memorymanager.h
    ${PROJECT_SOURCE_DIR}/memorymanager.cpp
)

add_phase0_test(tst_configmanager
    tst_configmanager.cpp
    ${PROJECT_SOURCE_DIR}/configmanager.h
    ${PROJECT_SOURCE_DIR}/configmanager.cpp
    ${PROJECT_SOURCE_DIR}/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.cpp
)

add_phase0_test(tst_memorypolicy
    tst_memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/memorypolicy.h
    ${PROJECT_SOURCE_DIR}/memorypolicy.cpp
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/qqmessageparser.cpp
)

add_phase0_test(tst_sessionrouter
    tst_sessionrouter.cpp
    ${PHASE0_CORE_SOURCES}
)

add_phase0_test(tst_promptassembler
    tst_promptassembler.cpp
    ${PROJECT_SOURCE_DIR}/promptassembler.h
    ${PROJECT_SOURCE_DIR}/promptassembler.cpp
)

add_phase0_test(tst_qqreplypolicy
    tst_qqreplypolicy.cpp
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.h
    ${PROJECT_SOURCE_DIR}/qqreplypolicy.cpp
)

add_phase0_test(tst_qqmessageparser
    tst_qqmessageparser.cpp
    ${PROJECT_SOURCE_DIR}/qqmessageparser.h
    ${PROJECT_SOURCE_DIR}/qqmessageparser.cpp
)

add_phase0_test(tst_globalmoodstate
    tst_globalmoodstate.cpp
    ${PROJECT_SOURCE_DIR}/globalmoodstate.h
    ${PROJECT_SOURCE_DIR}/globalmoodstate.cpp
)
