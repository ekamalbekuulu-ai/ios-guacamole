/* Firmware the GPU probe embeds (from the stock vendor image). */
#ifdef SHIM_GPUPROBE
	.section .rodata
	.balign 16
	.global gmu_fw, gmu_fw_end
gmu_fw:
	.incbin "fw/a640_gmu.bin"
gmu_fw_end:
	.balign 16
	.global zap_mdt, zap_mdt_end, zap_b02, zap_b02_end
zap_mdt:
	.incbin "fw/a640_zap.mdt"
zap_mdt_end:
	.balign 16
zap_b02:
	.incbin "fw/a640_zap.b02"
zap_b02_end:
	.balign 16
	.global sqe_fw, sqe_fw_end
sqe_fw:
	.incbin "fw/a630_sqe.fw"
sqe_fw_end:
#endif
