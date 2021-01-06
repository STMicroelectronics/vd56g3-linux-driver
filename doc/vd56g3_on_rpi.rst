================================
VD56G3 on RPI - Quickstart Guide
================================

Required Hardware
=================
- Raspberry PI 3 or 4 (tested on RPI3, should work similary on RPI4) + SD card + Power adapter
- ST Imaging Raspberry to SPIDER adapter (PCB1637-00B) + Flex cable
- ST Imaging FOX Demo OLGA80 (PCB1706B)
- Peripherals: Screen, Mouse, Keyboard, cables, etc.


Step-by-step guide
==================

#. Install linux on the RPI

    #. Retrieve lastest `raspbian image <https://downloads.raspberrypi.org/raspios_armhf/images/raspios_armhf-2020-12-04/2020-12-02-raspios-buster-armhf.zip>`_ .
    #. Flash the downloaded .img file on the micro SD card

        - On Windows, you can use `Win32 Disk Imager <https://sourceforge.net/projects/win32diskimager/>`_
        - On Linux, you can use ``dd`` with the following command line::

            dd if=2020-12-02-raspios-buster-armhf.img of=/dev/<your_sdcard_device> bs=1M


#. Linux customization on first boot

    .. Note::
        Depending of your network infrastructure, you may be required to setup proxy.
    
    #. Follow the RPI wizard to configure: locales, default password, network connection, etc.

    #. By the end of the wizard, update your system.

    #. Install linux kernel headers ::

        sudo apt install raspberrypi-kernel-headers

    #. You can also add interesting packages (will be used in next steps) ::

        sudo apt install vim qv4l2 yavta
        

#. Install last Fox driver 

    #. Retrieve the driver from the git repository::

        git clone ssh://gitolite@codex.cro.st.com/imgfox/linux/driver/vd56g3.git

    #. Build then install the driver as a module::

        cd vd56g3
        make
        sudo cp st-vd56g3.ko /lib/modules/`uname -r`/kernel/drivers/media/i2c/.
        sudo depmod -a


#. Update the Device Tree to describe the new Fox Setup

    .. Note ::
        The Raspberry ecosystem make big use of device tree overlays. On RPI, the overlays are precompiled and are located in the ``/boot/overlays`` folder.

       
    #. The ``vd56g3-overlay.zip`` provided contains two files :

        - ``vd56g3-overlay.dts`` the overlay source file
        - ``vd56g3.dtbo`` the compiled version of the ``vd56g3-overlay.dts`` file

        .. Important ::
            The ``vd56g3.dtbo`` overlay have been compiled for an RPI3. Due to GPIO differences between all RPI versions, this compiled version will probably not work on other RPI versions

    #. Once extracted, copy then enable the vd56g3 overlay ::

        # Copy the vd56g3 compiled overlay
        unzip vd56g3-overlay.zip
        sudo cp vd56g3.dtbo /boot/overlays/.
        # Enable vd56g3 overlay
        sudo sh -c 'echo "dtoverlay=vd56g3" >> /boot/config.txt'
    
    #. Finally you can reboot your RPI. Once rebooted you can check the driver status ::

            dmesg | grep vd56g3


#. Make the sensor stream ! There's a lot of ways to grab frames; 3 alternatives are described below

    #. The ``qv4l2`` utility provides a pannel to adjust all controls of the sensor (resolution, pixelformat, framerate, exposure, custom controls, etc.). 
       We can also make the sensor stream in a windows. Unfortunately the pixel format conversion (realized in SW) is horrible slow and the FPS won't go upper than 15FPS.

    #. With yavta, one can grab frames via commandline ::

        # Capture 20 frames @ 640x480, format SGBRG8, 90 FPS, save frame on disk
        yavta /dev/video0 --capture=20 --size 640x480 --format SRGGB8 --time-per-frame 1/90 --file 
        
    #. It exist a yavta fork customized for RPI. This version make use of RPI GPU features for pixel format conversion and it enables to display the frames on the screen ::

        # Retrieve the yavta fork
        git clone https://github.com/6by9/yavta.git

        # Build It
        cd yavta/
        make

        # Grab @1124x1364, 60FPS and display on screen
        ./yavta --capture=1000 --size 1124x1364 --format SRGGB8 /dev/video0 --time-per-frame 1/60 -m

About Optical-Flow
==================

The current Fox driver has a branch with OF support.

To enable OF, one must rebuild the driver and reinstall it ::

    # From the fox repository
    git checkout optical_flow
    make
    sudo cp st-vd56g3.ko /lib/modules/`uname -r`/kernel/drivers/media/i2c/.
    sudo depmod -a
    reboot

In the current driver implementation, the Optical Flow infos are passed at the end of the frame.
Frame sizes are extended by 64 lines. So you would be able to see new resolutions supported by the sensor; for example 640x544 (which is 640x480 + 640x64 for OF).

Howto grab ? Well we can use the same yavta tool like before :: 

    # Using the customized version of yavta, Grab @1024x1344 (1024x1280 + 64 lines of OF), 40FPS and display on screen
    ./yavta --capture=1000 --size 1024x1344 --format SRGGB8 /dev/video0 --time-per-frame 1/40 -m

    .. Note:: 
        With the previous command line the optical flow are interpreted as image data and are displayed as gibberish data at the end of the frame

.. Important::
    The OF grabbing is done using a debug feature of the sensor in order to have Image and OF on the same Channel/Datatype. 
    This was done because, there’s currently no Virtual Channel support on Linux V4L2 (or at least not mainstreamed).
    The backside is that using the FOX this way decrease the performances. We can’t reach nominal 60FPS in such debug mode. We should be able to reach 40FPS.