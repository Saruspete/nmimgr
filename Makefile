
obj-m += nmimgr.o

KVERS ?= $(shell uname -r)

.PHONY: all

all: $(KVERS)


clean: clean-$(KVERS)
#	make -C /lib/modules/$@/build M=$(PWD) clean
	
#cleanall: clean-$(KVERS)

clean-%:
	#cd nmimgr.kmod.$(@:clean-%=%) && make clean # make -C /lib/modules/$@/build M=$(PWD) clean
	$(eval kv=$(@:clean-%=%))
	make -C /lib/modules/$(kv)/build M=$(PWD)/nmimgr.kmod.$(kv) clean
	rm $(PWD)/nmimgr.kmod.$(kv)/{Makefile,nmimgr.c}
	rmdir $(PWD)/nmimgr.kmod.$(kv)


%:
	$(eval kv=$(@:clean-%=%))
	mkdir -p nmimgr.kmod.$@
	cp nmimgr.c Makefile nmimgr.kmod.$@
	make -C /lib/modules/$@/build M=$(PWD)/nmimgr.kmod.$@ modules
