============
Introduction
============

This document describes the **Linux VD56G3 driver** providing control over the VD56G3 : a portrait-oriented global shutter image sensor optimized for near infrared scenes.

.. Note ::

    VD56G3's features fully supported in the driver:

    - Mono (VD56G3) & RGB (VD66GY) versions
    - Full resolution : 1124 pixels x 1364 pixels (1.5 Mpixel)
    - Up to 88 fps at full resolution
    - RAW8 / RAW10 output
    - Embedded auto-exposure
    - Horizontal / Vertical Flip
    - Up to 8 illumination control outputs synchronized with sensor integration periods
    - Master/Slave mode (for start frame synchronization)

.. Warning ::

    VD56G3's features partially used in the driver (but not exposed):

    - Binning (x2 and x4)
    - Sub sampling (x2 and x4)
    - Crop


    VD56G3's features **NOT supported** in the driver:

    - Optical Flow
    - Programmable sequences of 4-frame contexts