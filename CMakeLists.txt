project(time-shift VERSION 1.0.0)
set(PROJECT_FULL_NAME "Time Shift")

set(time-shift_HEADERS
	time-shift.h
	easing.h)
set(time-shift_SOURCES
	time-shift.c
	time-glitch.c
	easing.c)

if(WIN32)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(resource.rc.in time-shift.rc)
	list(APPEND time-shift_SOURCES
		time-shift.rc)
endif()

add_library(time-shift MODULE
	${time-shift_HEADERS}
	${time-shift_SOURCES})
target_link_libraries(time-shift
	libobs)

set_target_properties(time-shift PROPERTIES FOLDER "plugins/exeldro")
install_obs_plugin_with_data(time-shift data)
