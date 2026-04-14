source ~/Git_repos/esp-idf/export.sh
idf.py set-target esp32                                                                                                                        
idf.py flash -p /dev/ttyUSB0

curl -s -X POST -H "Content-Type: text/plain" --data-binary @tests/gdb/test_program.hex                    
      http://192.168.0.2/api/firmware