#
# helper Makefile to build kernel device tree blob from bbappend source
# in yocto layer without bitabking kernel recipe (kernel must have been built 
# once before so kernel sourec exists in yocto tmp directory)
#

.SUFFIXES:
.SUFFIXES: .dts .dtb

DTS_CPP_ARGS = -nostdinc -I include -I arch  -undef -x assembler-with-cpp  

# add your yocto build path here
KERNEL_SRC = /home/m/02_Workspaces/2105_Aries-RISCV-Webinar/WS001/build/tmp-glibc/work-shared/m100pfsevp/kernel-source/
DTS_PATH = arch/riscv/boot/dts/
DTS_PREFIX = ../../recipes-kernel/linux/mpfs-linux/m100pfsevp

DTS_PRE_INCLUDE_DIRS += include \
						$(DTS_PATH) \
						$(DTS_PATH)aries 

DTS_PRE_INCLUDES = $(addprefix -I $(KERNEL_SRC),$(DTS_PRE_INCLUDE_DIRS))


%.dts_pre: $(DTS_PREFIX)/%.dts
		$(CPP) $(DTS_PRE_INCLUDES) $(DTS_CPP_ARGS) $< > $@ 
		
%.dtb:  %.dts_pre
		dtc -I dts -O dtb -o $@ $<


