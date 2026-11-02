qt_add_executable(remclaude_gui_android ${PROJECT_SOURCE_DIR}/desktop/main.cpp)
remclaude_setup_target(remclaude_gui_android)

set_target_properties(remclaude_gui_android PROPERTIES
    QT_ANDROID_PACKAGE_NAME       "org.camradeling.remclaude"
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/package-source"
    QT_ANDROID_MIN_SDK_VERSION    28
    QT_ANDROID_VERSION_NAME       "0.1.0"
    QT_ANDROID_EXTRA_LIBS         "${REMCLAUDE_ANDROID_SSL_LIBS}"
)
