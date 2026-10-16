add_executable(tdlas_processing_worker_test
    processing_worker_test.cpp
)

target_link_libraries(tdlas_processing_worker_test
    PRIVATE
        tdlas_core
        Qt${QT_VERSION_MAJOR}::Core
)

if (MSVC)
    target_compile_options(tdlas_processing_worker_test PRIVATE /W4)
else()
    target_compile_options(tdlas_processing_worker_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tdlas_processing_worker_test COMMAND tdlas_processing_worker_test)

add_executable(tdlas_peak_data_saver_test
    peak_data_saver_test.cpp
)

target_link_libraries(tdlas_peak_data_saver_test
    PRIVATE
        tdlas_core
        Qt${QT_VERSION_MAJOR}::Core
)

target_compile_definitions(tdlas_peak_data_saver_test
    PRIVATE TDLAS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

if (MSVC)
    target_compile_options(tdlas_peak_data_saver_test PRIVATE /W4)
else()
    target_compile_options(tdlas_peak_data_saver_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tdlas_peak_data_saver_test COMMAND tdlas_peak_data_saver_test)
set_tests_properties(tdlas_peak_data_saver_test PROPERTIES TIMEOUT 30)

add_executable(tdlas_main_window_test
    main_window_test.cpp
)

target_link_libraries(tdlas_main_window_test PRIVATE tdlas_ui)

if (MSVC)
    target_compile_options(tdlas_main_window_test PRIVATE /W4)
else()
    target_compile_options(tdlas_main_window_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tdlas_main_window_test COMMAND tdlas_main_window_test)
set_tests_properties(tdlas_main_window_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)
