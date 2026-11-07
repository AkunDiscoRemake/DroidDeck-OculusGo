add_library(vrapi SHARED IMPORTED)
set_target_properties(vrapi PROPERTIES
        IMPORTED_LOCATION "${DROIDDECK_OVR_SDK_DIR}/VrApi/Libs/Android/${ANDROID_ABI}/Release/libvrapi.so")
add_library(droiddeckgo SHARED go_vr.cpp)
target_compile_features(droiddeckgo PRIVATE cxx_std_17)
target_compile_options(droiddeckgo PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Werror=return-type)
target_include_directories(droiddeckgo PRIVATE
        "${DROIDDECK_OVR_SDK_DIR}/VrApi/Include"
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/vr/src)
target_link_options(droiddeckgo PRIVATE -Wl,-z,max-page-size=16384 -uANativeActivity_onCreate)
target_link_libraries(droiddeckgo PRIVATE droiddeckxr_glue vrapi EGL GLESv3 android log)
