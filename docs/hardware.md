# Supported hardware

Currently this project supports two board types, but ali-express seems to have many similar cheap boards of different resolutions.  I'm happy to help anyone add a new supported board to this project - it is quite easy.  (Ping me if you want to do this and I'll flesh out these instructions).

Currently the following two boards are supported

* CYD "Cheap Yellow Display" boards such as jc4827w543 (costs just $15 USD!)
The JC4827W543 (often sold under names like Guition or Sunton) is an ESP32-S3 board paired with a 4.3" 480x272 RGB display (using an NV3041A or ST3401A controller) and usually a GT911 capacitive touch chip. For detailed hw docs see [here](https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board).

* The [Waveshare 7 inch](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7), eventually many other similar boards will be supported.  For more information on this board see https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B.  This board is slightly expensive but has a very high-res screen.  Costs about $30 USD on aliexpress.

## Typical links
(No endorsement of particular vendors is implied)

![sample hw](images/hw.png)

### Screen+hardware 
* https://www.aliexpress.us/item/3256808357526751.html
* https://shopee.tw/ESP32-P4-WIFI6-7%E8%8B%B1%E5%AF%B8%E4%BA%94%E9%BB%9E%E8%A7%B8%E6%8E%A7%E5%B1%8F%E9%96%8B%E7%99%BC%E6%9D%BF-1024%C3%97600%E5%88%86%E8%BE%A8%E7%8E%87-%E5%9F%BA%E6%96%BCESP32-P4%E5%92%8CESP32-C6-%E6%94%AF-i.871575797.40371503863
* https://shopee.tw/ESP32-S34.3%E5%AF%B85%E5%AF%B8LCD%E9%9B%BB%E5%AE%B9%E8%A7%B8%E6%91%B8%E5%B1%8FLVGL%E9%96%8B%E7%99%BC%E6%9D%BFIO%E6%93%B4%E5%B1%95-i.1578084179.44557771187?extraParams=%7B%22display_model_id%22%3A410687861211%2C%22model_selection_logic%22%3A3%7D&sp_atk=b636f0bc-1d2e-4669-8e72-0a2a2fb56695&xptdk=b636f0bc-1d2e-4669-8e72-0a2a2fb56695
* https://shopee.tw/-ESP32%E9%96%8B%E7%99%BC%E6%9D%BFWiFi%E8%97%8D%E7%89%992.8%E5%AF%B8240*320%E6%99%BA%E8%83%BD%E9%A1%AF%E7%A4%BA%E5%B1%8FTFT%E6%A8%A1%E5%A1%8A%E8%A7%B8%E6%91%B8%E8%9E%A2%E5%B9%95LVGL-i.265939604.43270466926?extraParams=%7B%22display_model_id%22%3A177318412563%2C%22model_selection_logic%22%3A3%7D&sp_atk=e2aa9b15-00cf-4c73-a053-7c6069bfe820&xptdk=e2aa9b15-00cf-4c73-a053-7c6069bfe820
* https://shopee.tw/ESP32-S3%E9%96%8B%E7%99%BC%E6%9D%BF%E5%B8%B65%E5%AF%B87%E5%AF%B8LCD%E5%9C%96%E5%BD%A2%E9%A1%AF%E7%A4%BA%E5%B1%8F%E9%9B%BB%E5%AE%B9%E5%B1%8FwifiMCU%E7%89%A9%E8%81%AF%E7%B6%B2-i.1725679736.56706409304

## More information

* [general info](https://www.espboards.dev/esp32/cyd-esp32-8048s050/) on these boards

## Haptics
We plan to support haptics in a future release (to provide nice button press 'feel')

Use this chip+motor TI DRV2605L https://www.adafruit.com/product/2305?srsltid=AfmBOopbcDSYZ7QqgK9jD2y2IHr5d2SBN0EGje71ejSgan9e44qDU4RJ 
https://www.aliexpress.us/w/wholesale-haptic-motor.html?spm=a2g0o.productlist.search.0

