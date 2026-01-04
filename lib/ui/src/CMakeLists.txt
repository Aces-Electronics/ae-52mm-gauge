SET(SOURCES ui_bootInitialScreen.c
    ui_batteryScreen.c
    ui_temperatureScreen.c
    ui_tpmsScreen.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_807091229.c
    ui_img_501072417.c
    ui_img_ae_white_128_png.c
    ui_img_1917549703.c
    ui_img_2056808699.c
    ui_img_1965588767.c
    ui_img_699809084.c
    ui_img_2930172.c
    ui_img_2022370193.c
    ui_img_1749172309.c
    ui_img_2105704960.c
    ui_img_1105680357.c
    ui_img_539538216.c
    ui_img_1209853138.c
    ui_img_2104900491.c
    ui_img_943648365.c
    fonts/ui_font_AE64BOLD.c)

add_library(ui ${SOURCES})
