# Release name
PRODUCT_RELEASE_NAME := k3gxx

# Inherit some common CM stuff.
$(call inherit-product, vendor/cm/config/common_full_phone.mk)

$(call inherit-product, vendor/cm/config/nfc_enhanced.mk)

# Inherit device configuration
$(call inherit-product, device/samsung/k3gxx/full_k3gxx.mk)
$(call inherit-product, device/samsung/k3gxx/system_prop.mk)

## Device identifier. This must come after all inclusions
PRODUCT_NAME := cm_k3gxx
PRODUCT_DEVICE := k3gxx
PRODUCT_BRAND := samsung
PRODUCT_MODEL=SM-G900H
PRODUCT_MANUFACTURER := samsung

# Set build fingerprint / ID / Product Name ect.

PRODUCT_BUILD_PROP_OVERRIDES += \
   PRODUCT_NAME="k3gxx" \
   PRIVATE_BUILD_DESC="k3gxx-user 6.0.1 MMB29K G900HXXS1CPJA release-keys" \
   BUILD_FINGERPRINT="samsung/k3gxx/k3g:6.0.1/MMB29K/G900HXXS1CPJA:user/release-keys"
