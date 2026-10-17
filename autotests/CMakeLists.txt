# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Compile the real engine implementation, without loading the kded plugin or
# starting a desktop session. No production code is replaced by test stubs.
add_library(vault_test_engine STATIC
    ../common/vaultinfo.cpp
    ../kded/engine/types.cpp
    ../kded/engine/commandresult.cpp
    ../kded/engine/backend_p.cpp
    ../kded/engine/fusebackend_p.cpp
    ../kded/engine/backends/cryfs/cryfsbackend.cpp
    ../kded/engine/backends/encfs/encfsbackend.cpp
    ../kded/engine/backends/gocryptfs/gocryptfsbackend.cpp
)
target_include_directories(vault_test_engine PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/kded/engine
)
target_link_libraries(vault_test_engine PUBLIC
    Qt::Core Qt::DBus Qt::Widgets
    KF6::ConfigCore KF6::ConfigGui KF6::I18n KF6::KIOCore KF6::KIOWidgets
)

add_executable(vault_test_helper backendhelper.cpp)
target_link_libraries(vault_test_helper PRIVATE Qt::Core)

foreach(test IN ITEMS coretest backendtest)
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE vault_test_engine Qt::Test)
    add_test(NAME vault_${test} COMMAND ${test})
    set_tests_properties(vault_${test} PROPERTIES LABELS "unit" TIMEOUT 45)
endforeach()
target_compile_definitions(backendtest PRIVATE VAULT_TEST_HELPER="$<TARGET_FILE:vault_test_helper>")
add_dependencies(backendtest vault_test_helper)
