idf_component_register(SRCS "app_dash.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp dash_data)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")

# ---------------------------------------------------------------------------
# 中文点阵字体: 编译前自动按场景内容裁剪子集
#
# 场景 JSON 里改了中文文案 -> 这里的 custom command 会重新跑 gen_dash_fonts.py,
# 重新裁一份只含用到字形的 TTF -> gspc 再把它烘进 GSPB。
# 也就是说"改了中文忘了重新生成字体"这件事在 Mosaico 版本里不存在了。
#
# 源字体默认用开发机上的 Noto Sans SC; 换机器时用 -DDASH_FONT_SOURCE=... 覆盖。
# ---------------------------------------------------------------------------
set(DASH_FONT_SOURCE "${DASH_FONT_SOURCE}" CACHE FILEPATH
    "生成中文子集用的源字体(Noto Sans SC)")
if(NOT DASH_FONT_SOURCE)
    set(DASH_FONT_SOURCE "/home/openclaw/esp/fonts/NotoSansSC-Regular.otf")
endif()
set(DASH_SCENE "${CMAKE_CURRENT_LIST_DIR}/ui/main.json")
set(DASH_FONT_GEN "${CMAKE_CURRENT_LIST_DIR}/../../tools/fonts/gen_dash_fonts.py")
set(DASH_FONT_SUBSET "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/NotoSansSC-DashSubset.ttf")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT "${DASH_FONT_SUBSET}"
    COMMAND ${Python3_EXECUTABLE} "${DASH_FONT_GEN}"
        --scene "${DASH_SCENE}"
        --source "${DASH_FONT_SOURCE}"
        --output "${DASH_FONT_SUBSET}"
    DEPENDS "${DASH_FONT_GEN}" "${DASH_SCENE}"
    COMMENT "Generating CJK font subset for the GSP scene"
    VERBATIM)
add_custom_target(dash_fonts DEPENDS "${DASH_FONT_SUBSET}")

mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json")

# 让 gspc 一定在字体生成之后跑, 否则会读到上一版的 TTF
add_dependencies(${COMPONENT_LIB}_gsp_bundle dash_fonts)
