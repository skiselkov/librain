#!/bin/bash
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#
# Copyright 2019 Saso Kiselkov. All rights reserved.

case $(uname) in
Linux)
	SED=sed
	;;
Darwin)
	SED=gsed
	;;
esac

"$SED" -i '
s/\<textureSize\>/textureSize2D/;
' $@
