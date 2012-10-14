NUT Container Format
====================

This is a personal try to cleanup a good container format with interesting
perks and good performance.

Possibly more effort will be poured on TransOgg or in a NUT2 variant once
the current NUT is specified properly and quirks will need to be ironed out.


Features
--------

Nut is low overhead, offers precise seeking and supports in a quite flexible
way most raw audio and video formats.


Warning
-------

    IT IS NOT ADVISED TO USE NUT FOR PERMANENT STORAGE DUE ITS UNDERSPECIFIED
    NATURE. DIFFERENT IMPLEMENTATIONS ALREADY MIGHT NOT ACCEPT FILES DEEMED
    VALID BY OTHERS.


Other reference
---------------

[Libav](http://libav.org/) and [FFmpeg](http://ffmpeg.org/) have two
indipendent implementation, mostly interoperable since the code is mostly the same.
The [original](svn://svn.mplayerhq.hu/nut) repository contains codec tags
updates from [FFmpeg](http://ffmpeg.org/), hopefully discrepancies will not
arise. The initial focus of this repository is mostly cleanup.
