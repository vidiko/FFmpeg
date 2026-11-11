FATE_HWCONTEXT += fate-hwdevice
fate-hwdevice: libavutil/tests/hwdevice$(EXESUF)
fate-hwdevice: CMD = run libavutil/tests/hwdevice$(EXESUF)
fate-hwdevice: CMP = null

FATE_HW-$(CONFIG_CUDA) += fate-hwcontext-cuda
fate-hwcontext-cuda: libavutil/tests/hwcontext_cuda$(EXESUF)
fate-hwcontext-cuda: CMD = run libavutil/tests/hwcontext_cuda$(EXESUF)
fate-hwcontext-cuda: CMP = grep
fate-hwcontext-cuda: REF = ^\(source reuse checksum: 178f1010\|No CUDA device available, skipping\.\)$$

FATE_HW-$(call ALLYES, HQDN3D_VULKAN_FILTER HQDN3D_FILTER) += fate-filter-hqdn3d-vulkan
fate-filter-hqdn3d-vulkan: libavfilter/tests/hqdn3d_vulkan$(EXESUF)
fate-filter-hqdn3d-vulkan: CMD = run libavfilter/tests/hqdn3d_vulkan$(EXESUF)

FATE_HW-$(CONFIG_AVUTIL) += $(FATE_HWCONTEXT)
