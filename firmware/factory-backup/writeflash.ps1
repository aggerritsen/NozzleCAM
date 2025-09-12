
# write back the exact image
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" `
  "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" `
  --chip esp32s3 --port COM11 --baud 921600 write_flash --flash_size detect 0x000000 factory_ttgo_tjournal_4MB.bin
