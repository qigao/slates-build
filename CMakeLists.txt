cmake_minimum_required(VERSION 3.27)
project(slate VERSION 0.1.0 LANGUAGES C)

find_package(
	SaltsBase
	0.1.8
	EXACT CONFIG REQUIRED
	HINTS "$ENV{SALTS_BASE_ROOT}"
)
if(WIN32 AND (MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
	set(core_sdk_properties IMPORTED_IMPLIB IMPORTED_LOCATION)
	get_target_property(core_sdk_configs Salts::Core IMPORTED_CONFIGURATIONS)
	foreach(config IN LISTS core_sdk_configs)
		list(APPEND core_sdk_properties IMPORTED_IMPLIB_${config} IMPORTED_LOCATION_${config})
	endforeach()
	foreach(property IN LISTS core_sdk_properties)
		get_target_property(core_sdk_library Salts::Core ${property})
		if(core_sdk_library MATCHES "\\.a$")
			message(FATAL_ERROR
				"Salts::Core uses the MinGW ABI and the selected compiler targets MSVC. "
				"Use the LLVM-MinGW configure task for this SDK or select matching MSVC SDKs.")
		endif()
	endforeach()
endif()
find_package(
	SaltsUtils
	0.1.6
	EXACT CONFIG REQUIRED
	HINTS "$ENV{SALTS_UTILS_ROOT}"
)
# The installed CMeta package locates its matching SaltsBase through this variable.
set(saved_salts_base_root "$ENV{SALTS_BASE_ROOT}")
set(ENV{SALTS_BASE_ROOT} "${SaltsBase_DIR}")
find_package(CMeta 0.1.8 EXACT CONFIG REQUIRED HINTS "$ENV{CMETA_ROOT}")
set(ENV{SALTS_BASE_ROOT} "${saved_salts_base_root}")

include(GNUInstallDirs)
set(project_template_dir "build/templates")
set(template_bundle_name "slates")
set(build_template_dir "share/${template_bundle_name}")
set(template_dir "${CMAKE_CURRENT_SOURCE_DIR}/${build_template_dir}")
set(install_template_dir "${CMAKE_INSTALL_DATADIR}/${template_bundle_name}")
file(RELATIVE_PATH
	installed_template_path
	"${CMAKE_INSTALL_FULL_BINDIR}"
	"${CMAKE_INSTALL_FULL_DATADIR}/${template_bundle_name}"
)
file(GLOB templates CONFIGURE_DEPENDS "${template_dir}/*.j2")
list(APPEND templates "${template_dir}/ninja-rules.json")
configure_file(src/template_paths.h.in generated/template_paths.h @ONLY)

add_executable("${PROJECT_NAME}"
	src/main.c
	src/support.c
	src/preset.c
	src/module.c
	src/graph.c
	src/package.c
	src/sdk.c
	src/ninja.c
	src/install.c
	src/test.c
	src/template.c
)
target_compile_features("${PROJECT_NAME}" PRIVATE c_std_11)
target_compile_definitions("${PROJECT_NAME}" PRIVATE
	SLATE_NAME="${PROJECT_NAME}"
	SLATE_VERSION="${PROJECT_VERSION}"
)
target_include_directories("${PROJECT_NAME}"
	PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries("${PROJECT_NAME}"
	PRIVATE
		Salts::Core
		Salts::CmdParser
		Salts::JsonParser
		Salts::Mustache
		Salts::JinjaCMeta
)
if(WIN32)
	target_link_libraries("${PROJECT_NAME}" PRIVATE shell32)
endif()
set_target_properties("${PROJECT_NAME}" PROPERTIES C_EXTENSIONS OFF)

if(MSVC)
	target_compile_options("${PROJECT_NAME}" PRIVATE /W4)
else()
	target_compile_options("${PROJECT_NAME}" PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(template_staging_target "${PROJECT_NAME}-templates")
set(staged_template_dir
	"$<TARGET_FILE_DIR:${PROJECT_NAME}>/${build_template_dir}"
)
add_custom_target("${template_staging_target}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${staged_template_dir}"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${templates}
		"${staged_template_dir}"
	DEPENDS ${templates}
	VERBATIM
)
add_dependencies("${PROJECT_NAME}" "${template_staging_target}")
install(FILES ${templates} DESTINATION "${install_template_dir}")
install(TARGETS "${PROJECT_NAME}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
	foreach(sdk_target Salts::Core Salts::JinjaCMeta)
		get_target_property(sdk_type "${sdk_target}" TYPE)
		if(sdk_type STREQUAL "SHARED_LIBRARY")
			add_custom_command(
				TARGET "${PROJECT_NAME}"
				POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"$<TARGET_FILE:${sdk_target}>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
				VERBATIM
			)
			install(
				FILES "$<TARGET_FILE:${sdk_target}>"
				DESTINATION "${CMAKE_INSTALL_BINDIR}"
			)
		endif()
	endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
	find_program(NINJA_EXECUTABLE ninja)
	if(NINJA_EXECUTABLE)
		add_test(
			NAME native-build
			COMMAND "${CMAKE_COMMAND}" "-DBUILDER=$<TARGET_FILE:${PROJECT_NAME}>"
				"-DNINJA=${NINJA_EXECUTABLE}" "-DCC=${CMAKE_C_COMPILER}"
				"-DAR=${CMAKE_AR}"
				"-DHOST_ARCH=${CMAKE_HOST_SYSTEM_PROCESSOR}"
				"-DCOMPILER_ID=${CMAKE_C_COMPILER_ID}" "-DMSVC_DRIVER=${MSVC}"
				"-DTEMPLATE=${template_dir}/build.ninja.j2"
				"-DTEST_INSTALL_TREE=${CMAKE_CURRENT_BINARY_DIR}"
				"-DTEST_INSTALL_CONFIG=$<CONFIG>"
				"-DTEST_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
				"-DTEST_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
				"-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixture 中文 with spaces" -P
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/native.cmake"
		)
	endif()
endif()
