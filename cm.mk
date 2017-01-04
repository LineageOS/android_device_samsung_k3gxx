# Release name
PRODUCT_RELEASE_NAME := k3gxx

# Inherit some common CM stuff.
$(call inherit-product, vendor/cm/config/common_full_phone.mk)

# Inherit device configuration
$(call inherit-product, device/samsung/k3gxx/device.mk)

## Device identifier. This must come after all inclusions
PRODUCT_NAME := cm_k3gxx
PRODUCT_DEVICE := k3gxx
PRODUCT_BRAND := Samsung
PRODUCT_MODEL := SM-G900H
PRODUCT_MANUFACTURER := Samsung
BOARD_VENDOR := Samsung
TARGET_VENDOR := Samsung
