set(srcs
    "app_main.c"
    "bench.c"
    "camera/app_video.c"
    "camera/ov5647_ctl.c"
    "camera/cam_ctrl.c"
    "pipeline/auto_exposure.c"
    "pipeline/fx_parallel.c"
    "pipeline/frame_pipeline.c"
    "storage/capture.c"
    "storage/avi_writer.c"
    "storage/mp4_writer.c"
    "storage/sd_writer.c"
    "storage/sd_card.c"
    "storage/usb_storage.c"
    "ui/ui_live.c"
    "ui/gallery.c"
    "ui/ui_editor.c"
    "ui/ui_lvgl.c"
    "display/display.c"
    "system/settings.c"
    "system/remote.c"
    "system/presets.c"
    "system/sound.c"
    "system/battery.c"
    "input/buttons.c"
    "effects/fx_registry.c"
    "effects/fx_chanshift.c"
    "effects/fx_scanline.c"
    "effects/fx_bitcrush.c"
    "effects/fx_blocks.c"
    "effects/fx_wave.c"
    "effects/fx_pixelsort.c"
    "effects/fx_tracers.c"
    "effects/fx_hueshift.c"
    "effects/fx_kaleido.c"
    "effects/fx_diffract.c"
    "effects/fx_drift.c"
    "effects/fx_vhs.c"
    "effects/fx_slitscan.c"
    "effects/fx_databend.c"
    "effects/fx_squint.c"
    "effects/fx_starburst.c"
    "effects/fx_lighttrails.c"
    "effects/fx_neon.c"
    "effects/fx_palette.c"
    "effects/fx_vangogh.c"
    "effects/fx_dream.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "." "camera" "pipeline" "storage" "ui" "input" "effects" "display" "system")

# LVGL 9.5 emits attribute/cpp warnings with the P4 toolchain; keep them from failing the build.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
