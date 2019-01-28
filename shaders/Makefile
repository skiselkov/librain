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
# Copyright 2018 Saso Kiselkov. All rights reserved.

SPVS = \
    generic.vert.spv \
    ws_temp.frag.spv \
    nil.frag.spv \
    ice_depth.vert.spv \
    ice_depth_add_point.frag.spv \
    ice_depth_remove_point.frag.spv \
    ice_depth_deice_point.frag.spv \
    ice_depth_add_line.frag.spv \
    ice_depth_remove_line.frag.spv \
    ice_depth_deice_line.frag.spv \
    ice_norm.frag.spv \
    ice_render.frag.spv \
    ice_blur.frag.spv \
    rain_stage1.frag.spv \
    rain_stage2.frag.spv \
    rain_stage2_comp.frag.spv \
    ws_rain.frag.spv \
    ws_smudge.frag.spv \
    droplets.comp.spv

OUTDIR=bin
SPIRVX_TGT_VERSION=120
SPIRVX_420_VERSION=420
SPIRVX_430_VERSION=430

GLSLANG=glslang
GLSL_CLEANUP=./glsl_cleanup.sh

ifeq ($(V),1)
	VERB=
define logMsg
endef
else	# Not Verbose
	VERB=@
define logMsg
	@echo $(1)
endef
endif

define BUILD_SHADER
	$(call logMsg,-n \	[GLSLANG]\	)
	$(VERB) $(GLSLANG) $(2) -G -o $@ $^

	$(call logMsg,\	[SPIRVX]\	$@)
	$(VERB) spirv-cross --version $(SPIRVX_TGT_VERSION) \
	    --extension GL_EXT_gpu_shader4 \
	    --output $(@:%.$(1).spv=%.$(1).glsl) $@
	$(VERB) $(GLSL_CLEANUP) $(@:%.$(1).spv=%.$(1).glsl)

	$(call logMsg,\	[SPIRVX 4.20]\	$(@:%.$(1).spv=%.$(1).glsl420))
	$(VERB) spirv-cross --version $(SPIRVX_420_VERSION) \
	    --output $(@:%.$(1).spv=%.$(1).glsl420) $@
	$(VERB) $(GLSL_CLEANUP) $(@:%.$(1).spv=%.$(1).glsl420)
endef

define BUILD_COMP_SHADER
	$(call logMsg,-n \	[GLSLANG]\	)
	$(VERB) $(GLSLANG) $(2) -G -S comp -o $@ $^

	$(call logMsg,\	[SPIRVX 4.30]\	$(@:%.$(1).spv=%.$(1).glsl430))
	$(VERB) spirv-cross --version $(SPIRVX_430_VERSION) \
	    --output $(@:%.$(1).spv=%.$(1).glsl430) $@
endef

SPVS_OUT=$(addprefix $(OUTDIR)/,$(SPVS))
all : $(SPVS_OUT)

clean :
	rm -f $(SPVS_OUT) $(patsubst %.spv,%.glsl,$(SPVS_OUT))

$(OUTDIR)/ice_depth_add_point.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=1 -DSRC_TYPE=0)
$(OUTDIR)/ice_depth_remove_point.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=0 -DSRC_TYPE=0 -DDEICE=0)
$(OUTDIR)/ice_depth_deice_point.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=0 -DSRC_TYPE=0 -DDEICE=1)
$(OUTDIR)/ice_depth_add_line.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=1 -DSRC_TYPE=1)
$(OUTDIR)/ice_depth_remove_line.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=0 -DSRC_TYPE=1 -DDEICE=0)
$(OUTDIR)/ice_depth_deice_line.frag.spv : ice_depth.frag
	$(call BUILD_SHADER,frag,-DADD_ICE=0 -DSRC_TYPE=1 -DDEICE=1)

$(OUTDIR)/%.vert.spv : %.vert
	$(call BUILD_SHADER,vert)

$(OUTDIR)/%.frag.spv : %.frag
	$(call BUILD_SHADER,frag)

$(OUTDIR)/%.comp.spv : %.glsl
	$(call BUILD_COMP_SHADER,comp)

$(OUTDIR)/rain_stage2_comp.frag.spv : rain_stage2.frag
	$(call BUILD_SHADER,frag,-DCOMPUTE_VARIANT=1)

$(addprefix $(OUTDIR)/,$(SPVS)) : | $(OUTDIR)

$(OUTDIR) :
	mkdir -p $(OUTDIR)
