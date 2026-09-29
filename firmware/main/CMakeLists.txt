set(srcs
    "app_main.c"
    "bench.c"
    "camera/app_video.c"
    "camera/ov5647_ctl.c"
    "pipeline/auto_exposure.c"
    "pipeline/frame_pipeline.c"
    "storage/capture.c"
    "ui/ui_live.c"
    "ui/ui_lvgl.c"
    "display/display.c"
    "input/buttons.c"
    "effects/fx_registry.c"
    "effects/fx_chanshift.c"
    "effects/fx_scanline.c"
    "effects/fx_bitcrush.c"
    "effects/fx_blocks.c"
    "effects/fx_wave.c"
    "effects/fx_pixelsort.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "." "camera" "pipeline" "storage" "ui" "input" "effects" "display")

# LVGL 9.5 emits attribute/cpp warnings with the P4 toolchain; keep them from failing the build.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
