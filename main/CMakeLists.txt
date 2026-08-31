idf_component_register(
    SRCS "cat_cam.cpp"
    INCLUDE_DIRS "."
    REQUIRES cat_detect esp32-camera usb_device_uvc
)
