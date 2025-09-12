# use PlatformIO’s bundled Python + esptool.py
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" `
  "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" `
  --chip esp32s3 --port COM12 --baud 921600 read_flash 0x000000 0x400000 factory_ttgo_camplus_16MB.bin
