# OpenSSL for Android from the KDAB android_openssl submodule.
# libssh2 needs only libcrypto; link the static archive so nothing extra has
# to be bundled into the APK.
set(_kdab "${PROJECT_SOURCE_DIR}/third_party/android_openssl/ssl_3/${ANDROID_ABI}")
if(NOT EXISTS "${_kdab}/libcrypto.a")
    message(FATAL_ERROR "android_openssl not found at ${_kdab}. Run: git submodule update --init --depth 1")
endif()

set(OPENSSL_ROOT_DIR       "${_kdab}"                CACHE PATH     "" FORCE)
set(OPENSSL_INCLUDE_DIR    "${_kdab}/include"        CACHE PATH     "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${_kdab}/libcrypto.a"    CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY    "${_kdab}/libssl.a"       CACHE FILEPATH "" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

set(REMCLAUDE_ANDROID_SSL_LIBS "")
