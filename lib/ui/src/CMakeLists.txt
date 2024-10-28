SET(SOURCES screens/ui_bootInitialScreen.c
    screens/ui_batteryScreen.c
    screens/ui_oilScreen.c
    screens/ui_coolantScreen.c
    screens/ui_turboExhaustScreen.c
    ui.c
    ui_helpers.c
    images/ui_img_807091229.c
    images/ui_img_501072417.c
    images/ui_img_ae_white_128_png.c
    images/ui_img_2056808699.c
    images/ui_img_1105680357.c
    images/ui_img_2930172.c
    images/ui_img_1209853138.c
    images/ui_img_2104900491.c
    images/ui_img_943648365.c)

add_library(ui ${SOURCES})
