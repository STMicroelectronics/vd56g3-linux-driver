============
Introduction
============

This document describes the **Linux VD56G3 driver** providing control over the VD56G3 : a global shutter image sensor optimized for near infrared scenes.

.. Note ::

    VD56G3's features fully supported in the driver: 

    - Global shutter technology
    - Full resolution : 1124 pixels x 1364 pixels (1.5 Mpixel)
    - Up to 100 fps at full resolution
    - Automatic dark calibration
    - Embedded auto-exposure
    - Mirror/flip readout


.. Warning ::

    VD56G3's features partially supported in the driver: 

    - Binning (x2 and x4)
    - Sub sampling (x2 and x4)
    - Crop 
    - Optical Flow
    - 8 multiple function IO, dynamically programmable with frame contexts (GPIO, strobe pulse, pulse-width modulation, V sync)
    - Up to 8 illumination control outputs synchronized with sensor integration periods and master/slave external frame start
    - Programmable sequences of 4-frame contexts, including frame parameters
