cmd_Release/hidapi.a := rm -f Release/hidapi.a && ./gyp-mac-tool filter-libtool libtool  -static -o Release/hidapi.a Release/obj.target/hidapi/hidapi/mac/hid.o
