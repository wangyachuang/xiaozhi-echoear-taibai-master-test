file(GLOB_RECURSE SRC_UI ${CMAKE_SOURCE_DIR} "espnow_ctrl/*.c")

idf_component_register(SRCS "rotating_base.c" "lexin_font.c" ${SRC_UI}
                    INCLUDE_DIRS "." "espnow_ctrl")

# Function to get component library
function(get_component_library component_name output_var)
# Get the exact component name
idf_build_get_property(build_components BUILD_COMPONENTS)
set(TARGET_COMPONENT "")
foreach(COMPONENT ${build_components})
    if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
        set(TARGET_COMPONENT ${COMPONENT})
        break()
    endif()
endforeach()

# Get the component library
if(TARGET_COMPONENT STREQUAL "")
    message(FATAL_ERROR "Component '${component_name}' not found.")
else()
    idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
endif()
endfunction()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
${LVGL_LIB} 
PRIVATE 
    -DLV_LVGL_H_INCLUDE_SIMPLE
    -DLV_USE_DEMO_MUSIC
)

get_component_library("bsp" BSP_LIB)
target_compile_options(
${BSP_LIB}
PUBLIC
    "-DBSP_ESP_MAGNESCREEN_USE_YYSJ"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
