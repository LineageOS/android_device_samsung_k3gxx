###########################################################
### GRAPHICS
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
	ro.opengles.version=196608 \
	ro.bq.gpu_to_cpu_unsupported=1\
	ro.sf.lcd_density=432 \
	debug.hwc.force_gpu=1 \
	debug.hwc.winupdate=1 \
	debug.hwc.otf=1 

###########################################################
### WIFI
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
	wifi.interface=wlan0

###########################################################
### RADIO \ RIL
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
	rild.libargs=-d /dev/ttyS0 \
	rild.libpath=/system/lib/libsec-ril.so \
	ro.telephony.ril_class=SlteRIL \
	ro.ril.hsxpa=1 \
	ro.ril.gprsclass=10 \
	ro.ril.telephony.mqanelements=5

###########################################################
### NFC
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
	ro.nfc.sec_hal=true

###########################################################
### CAMERA
###########################################################

# This fixes switching between front/back camera sensors
PRODUCT_PROPERTY_OVERRIDES += \
	camera2.portability.force_api=1 \
	media.stagefright.legacyencoder=true \
	media.stagefright.less-secure=true

###########################################################
### DALVIK/ART
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
	dalvik.vm.heapstartsize=8m \
	dalvik.vm.heapgrowthlimit=256m \
	dalvik.vm.heapsize=512m \
	dalvik.vm.heaptargetutilization=0.75 \
	dalvik.vm.heapminfree=2m \
	dalvik.vm.heapmaxfree=8m

###########################################################
### HWUI
###########################################################

PRODUCT_PROPERTY_OVERRIDES += \
    	ro.hwui.texture_cache_size=50 \
	ro.hwui.layer_cache_size=34 \
	ro.hwui.path_cache_size=10 \
	ro.hwui.shape_cache_size=4 \
	ro.hwui.gradient_cache_size=2 \
	ro.hwui.drop_shadow_cache_size=6 \
	ro.hwui.text_small_cache_width=2048 \
	ro.hwui.text_small_cache_height=2048 \
	ro.hwui.text_large_cache_width=4096 \
	ro.hwui.text_large_cache_height=4096

