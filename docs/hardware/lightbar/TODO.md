
* fix background? bright white flash at startup
* !update readme
* add hw build instructions
* post to reddit
* property or name lookup api for changing field contents on the fly - possibly html dom like?
* sometimes seems a bit laggy - check timings to ensure we can achive 60fps on two panels per chain, two chains

The background color is a simple color to fill the display. It can be adjusted with lv_disp_set_bg_color(disp, color).
The opacity of the background color or image can be adjusted with lv_disp_set_bg_opa(disp, opa).
The disp parameter of these functions can be NULL to select the default display.