cmake_minimum_required(VERSION 3.22)
project(vhmdlink C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OPENXR_DIR: include/ and libopenxr_loader.so taken from the Khronos loader AAR
# by build_apk.py.
if(NOT OPENXR_DIR)
    message(FATAL_ERROR "set OPENXR_DIR (build_apk.py does)")
endif()

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION ${OPENXR_DIR}/libopenxr_loader.so)

add_library(vhmdlink SHARED src/main.cpp)
target_include_directories(vhmdlink PRIVATE ${OPENXR_DIR}/include)
target_compile_options(vhmdlink PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(vhmdlink PRIVATE native_app_glue openxr_loader android log EGL GLESv3 mediandk nativewindow aaudio)
# NativeActivity looks this symbol up by name; keep it although nothing calls it.
target_link_options(vhmdlink PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
