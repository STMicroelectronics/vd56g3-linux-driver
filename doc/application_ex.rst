====================
Application examples
====================

.. Note :: 
    From userspace perspective, the VD56G3 driver offer a sub-device node (``/dev/v4l-subdevXXX``) which can be used to configure and control the sensor.

The sensor can be controlled through `Mainstream V4L2 applications`_ or `Custom applications`_ (especially when the sensor provides specific features).

Mainstream V4L2 applications
============================

List of useful V4L2 applications:

:v4l2-ctl:
	+ Swiss army knife for v4l2 : can be used to query or configure a v4l2 device
	+ Provided by the ``v4l-utils`` package

    .. Warning :: 
        Some v4l2-ctl features are only available in recent versions. One may consider rebuild the last `v4l-utils version <https://www.linuxtv.org/downloads/v4l-utils/>`_.
         

:mediat-ctl:
	+ Userspace application that uses the Linux Media Controller API to configure pipelines. 
	+ Provided by the ``v4l-utils`` package

:qv4l2:
	+ Qt test application for V4L2
	+ Provided by the ``qv4l2`` package
	
:yavta:
	+ Yet Another V4L2 Test Applications
	+ Tool to test V4L2 devices in command line.
	+ It supports many of the latest V4L2 capabilities such as multi-plane capture, and many video formats. 
	+ Provided in the ``yavta`` package

:gstreamer:
	+ A pipeline-based multimedia framework that can be used to stream V4L2 devices
	+ Provided in the ``gstreamer`` package


Sensor configuration
--------------------

In its default configuration the sensor is ready to stream. But it's possible to adjust some configuration (see the list of :ref:`vd56g3_supported_crtls`). Many alternatives exist, 3 are described below.

#. The graphical way: ``qv4l2`` utility provides a pannel to adjust all controls of the sensor (resolution, pixelformat, framerate, exposure, custom controls, etc.).

#. ``v4l2-ctl`` command line utility can be used to configure the v4l2 sensor. ::

    # List all controls and their menus (need to identify first the subdevice node)
    v4l2-ctl --device /dev/v4l-subdev11 --list-ctrls-menus

    # Get value for 'test_pattern' control
    v4l2-ctl --device /dev/v4l-subdev11 --get-ctrl test_pattern

    # Set value '1' for 'test_pattern' control
    v4l2-ctl --device /dev/v4l-subdev11 --set-ctrl test_pattern=1  

#. ``yavta`` command line tool can also be used for sensor configuration. ::

    # List all sub-device controls (need to identify first the sub-device node)
    yavta --list-controls /dev/v4l-subdev10

    # Get value for control '0x009f0903' (correspond to the 'test_pattern' control)
    yavta --get-control 0x009f0903 /dev/v4l-subdev10

    # Set value '1' for control '0x009f0903' (correspond to the 'test_pattern' control)
    yavta --set-control 0x009f0903 1 /dev/v4l-subdev10

.. _sensor_streaming:    

Sensor streaming
----------------

.. Warning :: 
    Before streaming, the media pipeline must be setup correctly !
    
There are as many ways to make the sensor stream as there are V4L2 applications; 4 alternatives are described below

    #. ``qv4l2`` test bench can be used to make the sensor stream in a windows. If a pixel format conversion is necessary, the display FPS can be very slow (depending on the ressources available on the platform).

    #. With ``yavta`` command line tool, it's possible to grab frames via commandline ::

        # Capture 20 frames @ 640x480, format SGBRG8, 90 FPS, save frame on disk
        yavta /dev/video0 --capture=20 --size 640x480 --format SRGGB8 --time-per-frame 1/90 --file


    #. ``gstreamer`` is an extremely powerful framework although a bit complex to master. Below are a few one-liners ::

        # Grab a single image (640x480) and convert in jpg
        gst-launch-1.0 -vvv v4l2src device=/dev/video0 num-buffers=1 ! 'video/x-bayer,format=gbrg,width=640,height=480' ! bayer2rgb ! jpegenc ! filesink location=image01.jpg

        # Stream a video (640x480)
        gst-launch-1.0 -vvv v4l2src device=/dev/video0 ! 'video/x-bayer,format=gbrg,width=640,height=480' ! bayer2rgb ! videoconvert ! fpsdisplaysink video-sink=glimagesink


    #. ``v4l2-ctl`` command line can also be used to grab video frames ::

        # Capture 5 frames and save them in "file.raw"
        v4l2-ctl --device /dev/video0 --stream-mmap --stream-count=5 --stream-to=file.raw


Custom applications
===================

The `V4L2 API`_ is available from userspace. It's possible to develop custom v4l2 applications. 
Some examples are provided in the sections belows.

.. _V4L2 API: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html


Grabbing frames
---------------
Making the sensor stream is not difficult, but the developer has to give special attention to its capture platform specificities, and especially :

- `Supported I/O streaming methods`_ : :MMAP, USERPTR or DMABUF
- `Supported planar APIs`_ : Single or Multi Planar support

An example of grabbing application is provided in the ``examples`` folder and reproduced below.

.. literalinclude:: ./examples/mpane_capture.c
    :language: c

This example targets Multi Planar capture platforms. 

.. important ::
    In order to test the example, the media controller pipeline should be first setup correctly !

This example can be tested ::

    # Compilation
    gcc -Wall mpane_capture.c -o mpane_capture
    # Grab 10 frames (480x640) in raw8 and save them on drive
    ./mpane_capture

Gstreamer can be used to convert the frames in JPG::

    gst-launch-1.0 -vvv filesrc location=frame-000000.bin blocksize=307200 ! 'video/x-bayer,format=gbrg,width=640,height=480,framerate=1/1' ! bayer2rgb ! jpegenc ! filesink location=frame-000000.jpg

.. _`Supported I/O streaming methods`: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/io.html
.. _Supported planar APIs: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/planar-apis.html


Updating sensor controls
------------------------
It's possible to change the sensor's V4L2 controls through an application.

An example of application to change the sensors controls is provided in the ``examples`` folder and reproduced below.

.. literalinclude:: ./examples/update_v4lctrl.c
    :language: c

The snippet of code above, 1) enumerate all available controls, 2) change the value of ``V4L2_CID_HFLIP`` control.

This example can be easily tested ::

    # Build it
    gcc -Wall update_v4lctrl.c -o update_v4lctrl
    # Run it
    ./update_v4lctrl

Depending of how the V4L2 Controls are implemented in the driver, they can be changed while the sensor is streaming or not.
With the above example, the sensor must be stopped before the ``V4L2_CID_HFLIP`` control can be changed.