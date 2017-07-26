# Grapics
PRODUCT_PROPERTY_OVERRIDES += \
	ro.bq.gpu_to_cpu_unsupported=1 \
	debug.hwc.force_gpu=1 \
	debug.hwc.winupdate=1 \
	debug.hwc.otf=1 

# Open GL
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=196609

# Lcd density
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=480

# Randomly from stock
PRODUCT_PROPERTY_OVERRIDES += \
    ro.ril.gprsclass=10 \
    ro.ril.hsxpa=1 \
    ro.sec.fle.encryption=true \
    ro.secwvk=220

# Ril
PRODUCT_PROPERTY_OVERRIDES += \
	rild.libpath=/system/lib/libsec-ril.so \
    rild.libargs=-d /dev/ttyS0 \
    ro.telephony.ril_class=SlteRIL \
    ro.ril.telephony.mqanelements=5 \
    ro.telephony.default_network=0 \
    telephony.lteOnCdmaDevice=0

# Camera Hacks
PRODUCT_PROPERTY_OVERRIDES += \
	camera2.portability.force_api=1 \
	media.stagefright.legacyencoder=true \
	media.stagefright.less-secure=true

# Wifi
PRODUCT_PROPERTY_OVERRIDES += \
    wlan.wfd.hdcp=disable
