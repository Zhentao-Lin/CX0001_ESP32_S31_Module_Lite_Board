# section 09 main component: socket modes over the ESP32-S31's native Wi-Fi. esp_wifi_* calls
# come straight from IDF's esp_wifi (on-chip radio); no esp_wifi_remote / esp_hosted / C6.
# main has implicit access to all components, so no REQUIRES needed -
# that also covers the driver/usb_serial_jtag*.h used by the TCP modes' console input, which is
# already part of the build through the USB-Serial/JTAG console. Declaring PRIV_REQUIRES here
# would take that implicit access away and break unrelated includes (esp_netif.h and friends).
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS ".")
