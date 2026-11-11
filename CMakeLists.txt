cmake_minimum_required(VERSION 3.26)

set(PLATFORM "" CACHE STRING "Target platform: macos, linux or windows")
set(ARCH "" CACHE STRING "Target architecture: arm64, x64, aarch64 or x86")
if(NOT PLATFORM OR NOT ARCH)
	message(FATAL_ERROR "Configure with -DPLATFORM=<macos|linux|windows> -DARCH=<arm64|x64|aarch64|x86>")
endif()

if(PLATFORM STREQUAL "macos")
	if(ARCH STREQUAL "arm64")
		set(CMAKE_OSX_ARCHITECTURES arm64)
	else()
		set(CMAKE_OSX_ARCHITECTURES x86_64)
	endif()
	set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(READ src/WebViewVersion.h version)
string(REGEX MATCH "WEBVIEW_PLUGIN_VERSION_MAJOR[ ]+([0-9]+)" _tmp ${version})
set(VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "WEBVIEW_PLUGIN_VERSION_MINOR[ ]+([0-9]+)" _tmp ${version})
set(VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "WEBVIEW_PLUGIN_VERSION_PATCH[ ]+([0-9]+)" _tmp ${version})
set(VERSION_PATCH "${CMAKE_MATCH_1}")

project(vpx_webview_plugin VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}" LANGUAGES C CXX)
if(APPLE)
	enable_language(OBJCXX)
endif()

if(CMAKE_BUILD_TYPE)
	set(EXTERNAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
	set(EXTERNAL_BUILD_TYPE "Release")
endif()
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external/${PLATFORM}-${ARCH}/${EXTERNAL_BUILD_TYPE}")
set(CEF_ROOT "${EXTERNAL_DIR}/cef")

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third-party/include/plugins/VPXPlugin.h" OR NOT EXISTS "${CEF_ROOT}/include/cef_app.h")
	message(FATAL_ERROR "External dependencies not found, run platforms/${PLATFORM}-${ARCH}/external.sh first")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
	add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_library(CEF_WRAPPER NAMES cef_dll_wrapper libcef_dll_wrapper
	PATHS "${CEF_ROOT}/build/libcef_dll_wrapper" PATH_SUFFIXES "${EXTERNAL_BUILD_TYPE}" NO_DEFAULT_PATH REQUIRED)

add_library(WebViewPlugin MODULE
	src/Audio.cpp
	src/CefHost.cpp
	src/Controller.cpp
	src/Pages.cpp
	src/TableBridge.cpp
	src/Util.cpp
	src/WebViewPlugin.cpp)
set_target_properties(WebViewPlugin PROPERTIES PREFIX "" OUTPUT_NAME "plugin-webview")
target_include_directories(WebViewPlugin PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/third-party/include"
	"${CEF_ROOT}")
target_link_libraries(WebViewPlugin PRIVATE "${CEF_WRAPPER}")

add_library(WebViewHelperObjects OBJECT src/helper/HelperMain.cpp)
target_include_directories(WebViewHelperObjects PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CEF_ROOT}")

set(STAGE_DIR "${CMAKE_BINARY_DIR}/webview")

if(APPLE)
	target_sources(WebViewPlugin PRIVATE src/CefMac.mm)
	set_target_properties(WebViewPlugin PROPERTIES SUFFIX ".dylib")
	target_link_libraries(WebViewPlugin PRIVATE "-framework AppKit")

	set(BUNDLE_DIR "${STAGE_DIR}/WebView.app/Contents")
	set(BUNDLE_FRAMEWORKS_DIR "${BUNDLE_DIR}/Frameworks")
	set(HELPER_TARGETS)
	set(HELPER_STAGE_COMMANDS)
	foreach(VARIANT "" "Alerts" "GPU" "Plugin" "Renderer")
		if(VARIANT STREQUAL "")
			set(HELPER_NAME "WebView Helper")
			set(HELPER_TARGET WebViewHelper)
			set(HELPER_ID "org.vpinball.webview.helper")
		else()
			set(HELPER_NAME "WebView Helper (${VARIANT})")
			set(HELPER_TARGET WebViewHelper${VARIANT})
			string(TOLOWER "${VARIANT}" VARIANT_ID)
			set(HELPER_ID "org.vpinball.webview.helper.${VARIANT_ID}")
		endif()
		add_executable(${HELPER_TARGET} MACOSX_BUNDLE $<TARGET_OBJECTS:WebViewHelperObjects>)
		set_target_properties(${HELPER_TARGET} PROPERTIES
			OUTPUT_NAME "${HELPER_NAME}"
			MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/helper/Info.plist.in"
			MACOSX_BUNDLE_GUI_IDENTIFIER "${HELPER_ID}")
		target_link_libraries(${HELPER_TARGET} PRIVATE "${CEF_WRAPPER}")
		list(APPEND HELPER_TARGETS ${HELPER_TARGET})
		list(APPEND HELPER_STAGE_COMMANDS
			COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:${HELPER_TARGET}>" "${BUNDLE_FRAMEWORKS_DIR}/${HELPER_NAME}.app"
			COMMAND codesign --force --sign - "${BUNDLE_FRAMEWORKS_DIR}/${HELPER_NAME}.app")
	endforeach()

	add_custom_target(webview ALL
		COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STAGE_DIR}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/licenses"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/src/plugin.cfg" "$<TARGET_FILE:WebViewPlugin>" "${STAGE_DIR}/"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Release/Chromium Embedded Framework.framework" "${BUNDLE_FRAMEWORKS_DIR}/Chromium Embedded Framework.framework"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/src/Bundle.plist" "${BUNDLE_DIR}/Info.plist"
		${HELPER_STAGE_COMMANDS}
		COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/LICENSE" "${STAGE_DIR}/licenses/license.txt"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CEF_ROOT}/LICENSE.txt" "${STAGE_DIR}/licenses/cef.txt"
		COMMENT "Staging webview plugin folder at ${STAGE_DIR}"
		VERBATIM)
	add_dependencies(webview WebViewPlugin ${HELPER_TARGETS})
else()
	find_library(CEF_LIBRARY NAMES cef libcef PATHS "${CEF_ROOT}/Release" NO_DEFAULT_PATH REQUIRED)
	target_link_libraries(WebViewPlugin PRIVATE "${CEF_LIBRARY}")
	if(WIN32)
		add_executable(WebViewHelper WIN32 $<TARGET_OBJECTS:WebViewHelperObjects>)
	else()
		add_executable(WebViewHelper $<TARGET_OBJECTS:WebViewHelperObjects>)
		set_target_properties(WebViewPlugin WebViewHelper PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
	endif()
	set_target_properties(WebViewHelper PROPERTIES OUTPUT_NAME "webview-helper")
	target_link_libraries(WebViewHelper PRIVATE "${CEF_WRAPPER}" "${CEF_LIBRARY}")

	add_custom_target(webview ALL
		COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STAGE_DIR}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/licenses"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/src/plugin.cfg" "$<TARGET_FILE:WebViewPlugin>" "$<TARGET_FILE:WebViewHelper>" "${STAGE_DIR}/"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Release" "${STAGE_DIR}"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Resources" "${STAGE_DIR}"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/LICENSE" "${STAGE_DIR}/licenses/license.txt"
		COMMAND "${CMAKE_COMMAND}" -E copy "${CEF_ROOT}/LICENSE.txt" "${STAGE_DIR}/licenses/cef.txt"
		COMMENT "Staging webview plugin folder at ${STAGE_DIR}"
		VERBATIM)
	add_dependencies(webview WebViewPlugin WebViewHelper)
endif()
