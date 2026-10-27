/*
 * SweetAccelerator user client: the door Metal talks through.
 *
 * SweetAccelerator.cpp publishes the IOService with MetalPluginName=SweetMetal;
 * this class is what SweetMetal.bundle opens with IOServiceOpen() and drives.
 * The selector numbers and the scalar in/out layout are exactly the flame /
 * rmx1931 contract so their phone/metal driver works unmodified:
 *
 *   0  params                      -> [gpu_id, chip_id, ready]
 *   1  alloc(size)                 -> [handle, gpu_va, size]   (+ mmap by handle)
 *   2  free(handle)
 *   3  import(surface id)          -> [surface id, gpu_va, size]
 *   4  submit(va, dwords, timeout) -> [fence]
 *   5  cache_flush(handle, off, len)
 *   6  submit_async(va, dwords)    -> [fence]
 *   7  wait_fence(fence, timeout)
 *
 * mmap: clientMemoryForType(type == handle) returns a descriptor over that
 * buffer's kernel memory, so IOConnectMapMemory64 gives the client a CPU
 * mapping of the same memory the GPU reads.
 */
#if defined(ARM64_BOARD_CONFIG_GUACAMOLE)

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <vm/pmap.h>
/* SweetSurface.h lives in iokit/Kernel/arm (patch 0046) and this file compiles
 * from bsd/dev, where neither the search path nor the compiler's per-file
 * directory finds a bare "SweetSurface.h". Keep it relative rather than adding
 * an include path to the kernel build. */
#include "../../iokit/Kernel/arm/SweetSurface.h"

extern "C" {
int sweet_a6xx_bo_alloc(uint64_t size, uint64_t *h, uint64_t *gpu_va, uint64_t *sz);
int sweet_a6xx_bo_free(uint64_t h);
int sweet_a6xx_bo_info(uint64_t h, vm_offset_t *kva, uint64_t *sz);
int sweet_a6xx_submit(uint64_t gpu_va, uint32_t dwords, uint32_t timeout_ms, uint32_t *fence);
int sweet_a6xx_wait_fence(uint32_t fence, uint32_t timeout_ms);
int sweet_a6xx_bo_cache_flush(uint64_t h, uint64_t off, uint64_t len);
int sweet_a6xx_surface_import_pages(uint32_t id, const uint64_t *pages, uint32_t npages,
    uint64_t *gpu_va, uint64_t *size);
bool sweet_gpu_ready(void);
}

extern "C" IOService *sweet_accel_create_user_client(IOService *provider);

class SweetAcceleratorClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetAcceleratorClient)
	/* IOUserClient declares this virtual but this XNU does not link its body;
	 * without an override the vtable keeps an undefined reference. */
	IOReturn exportObjectToClient(task_t, OSObject *, ipc_port_t *) APPLE_KEXT_OVERRIDE;

	IOService *fProvider;

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;

	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAcceleratorClient, IOUserClient)

IOReturn
SweetAcceleratorClient::exportObjectToClient(task_t, OSObject *, ipc_port_t *)
{
	return kIOReturnUnsupported;
}

bool
SweetAcceleratorClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fProvider = NULL;
	return true;
}

bool
SweetAcceleratorClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fProvider = provider;
	IOLog("SWEET MTL: user client opened\n");
	return true;
}

void
SweetAcceleratorClient::stop(IOService *provider)
{
	IOUserClient::stop(provider);
}

IOReturn
SweetAcceleratorClient::clientClose(void)
{
	terminate(kIOServiceSynchronous);
	return kIOReturnSuccess;
}

IOReturn
SweetAcceleratorClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;

	if (args == NULL) {
		return kIOReturnBadArgument;
	}
	if (!sweet_gpu_ready() && selector != 0) {
		return kIOReturnNotReady;
	}

	switch (selector) {
	case 0: {	/* params */
		if (args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		args->scalarOutput[0] = 640;		/* Adreno 640 */
		args->scalarOutput[1] = 0x06040001ULL;	/* guacamole chip id */
		args->scalarOutput[2] = sweet_gpu_ready() ? 1 : 0;
		args->scalarOutputCount = 3;
		return kIOReturnSuccess;
	}
	case 1: {	/* alloc */
		if (args->scalarInputCount < 1 || args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		uint64_t h = 0, gva = 0, sz = 0;
		if (sweet_a6xx_bo_alloc(args->scalarInput[0], &h, &gva, &sz)) {
			return kIOReturnNoMemory;
		}
		args->scalarOutput[0] = h;
		args->scalarOutput[1] = gva;
		args->scalarOutput[2] = sz;
		args->scalarOutputCount = 3;
		return kIOReturnSuccess;
	}
	case 2:		/* free */
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		return sweet_a6xx_bo_free(args->scalarInput[0]) ? kIOReturnNotFound : kIOReturnSuccess;

	case 3: {
		if (args->scalarInputCount < 1 || args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		uint32_t sid = (uint32_t)args->scalarInput[0];
		SweetSurface *s = sweet_surface_copy(sid);
		if (s == NULL) {
			return kIOReturnNotFound;
		}
		static uint64_t pages[1024];
		uint32_t n = 0;
		IOByteCount seg = 1;
		for (off_t off = 0; off < (off_t)s->allocSize && n < 1024 && seg; off += seg) {
			seg = 0;
			addr64_t pa = s->memory ? s->memory->getPhysicalSegment(off, &seg, kIODirectionInOut) : 0;
			if (pa == 0 || seg == 0) {
				break;
			}
			for (IOByteCount k = 0; k < seg && n < 1024; k += PAGE_SIZE) {
				pages[n++] = pa + k;
			}
		}
		uint64_t gva = 0, sz = 0;
		int rc = n ? sweet_a6xx_surface_import_pages(sid, pages, n, &gva, &sz) : -1;
		s->release();
		if (rc) {
			return kIOReturnError;
		}
		args->scalarOutput[0] = (uint64_t)sid;
		args->scalarOutput[1] = gva;
		args->scalarOutput[2] = sz;
		args->scalarOutputCount = 3;
		return kIOReturnSuccess;
	}

	case 4: {	/* submit and wait */
		if (args->scalarInputCount < 3 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		uint32_t fence = 0;
		IOReturn r = sweet_a6xx_submit(args->scalarInput[0], (uint32_t)args->scalarInput[1],
		    (uint32_t)args->scalarInput[2], &fence) ? kIOReturnError : kIOReturnSuccess;
		args->scalarOutput[0] = fence;
		args->scalarOutputCount = 1;
		return r;
	}
	case 5:		/* cache flush */
		if (args->scalarInputCount < 3) {
			return kIOReturnBadArgument;
		}
		return sweet_a6xx_bo_cache_flush(args->scalarInput[0], args->scalarInput[1],
		    args->scalarInput[2]) ? kIOReturnBadArgument : kIOReturnSuccess;

	case 6: {	/* submit, no wait */
		if (args->scalarInputCount < 2 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		uint32_t fence = 0;
		IOReturn r = sweet_a6xx_submit(args->scalarInput[0], (uint32_t)args->scalarInput[1],
		    500, &fence) ? kIOReturnError : kIOReturnSuccess;
		args->scalarOutput[0] = fence;
		args->scalarOutputCount = 1;
		return r;
	}
	case 7:		/* wait fence */
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		return sweet_a6xx_wait_fence((uint32_t)args->scalarInput[0],
		    (uint32_t)args->scalarInput[1]) ? kIOReturnTimeout : kIOReturnSuccess;

	default:
		return kIOReturnUnsupported;
	}
}

IOReturn
SweetAcceleratorClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	vm_offset_t kva = 0;
	uint64_t size = 0;

	if (memory == NULL) {
		return kIOReturnBadArgument;
	}
	if (sweet_a6xx_bo_info((uint64_t)type, &kva, &size) || kva == 0 || size == 0) {
		return kIOReturnNotFound;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddress((void *)kva, (IOByteCount)size,
	    kIODirectionInOut);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	*options = 0;
	*memory = md;		/* the caller consumes the reference */
	return kIOReturnSuccess;
}

IOService *
sweet_accel_create_user_client(IOService *provider)
{
	SweetAcceleratorClient *client = new SweetAcceleratorClient;
	if (client == NULL) {
		return NULL;
	}
	if (!client->init()) {
		client->release();
		return NULL;
	}
	if (!client->attach(provider)) {
		client->release();
		return NULL;
	}
	client->release();	/* attach retains */
	return client;
}

#endif /* ARM64_BOARD_CONFIG_GUACAMOLE */




