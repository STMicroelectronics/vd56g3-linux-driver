============
Introduction
============

This document describes the **Linux VD56G3 driver** providing control over the VD56G3 : a global shutter image sensor optimized for near infrared scenes.

.. Note ::

    VD56G3's features fully supported in the driver:

    - Mono (VD56G3) & RGB (VD66GY) versions
    - Full resolution : 1124 pixels x 1364 pixels (1.5 Mpixel)
    - Up to 88 fps at full resolution
    - RAW8 / RAW10 output
    - Embedded auto-exposure
    - Horizontal / Vertical Flip


.. Warning ::

    VD56G3's features partially supported in the driver:

    - Binning (x2 and x4)
    - Sub sampling (x2 and x4)
    - Crop
    - 8 multiple function IO, dynamically programmable with frame contexts (GPIO, strobe pulse, pulse-width modulation, V sync)
    - Up to 8 illumination control outputs synchronized with sensor integration periods and master/slave external frame start


    VD56G3's features **NOT supported** in the driver:

    - Optical Flow
    - Programmable sequences of 4-frame contexts, including frame parameters