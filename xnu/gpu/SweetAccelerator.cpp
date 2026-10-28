/*
 * The GPU as Metal sees it. Metal.framework (MTLRegisterDevices) iterates
 * IOServiceMatching("IOAcceleratorES"), loads
 * /System/Library/Extensions/<MetalPluginName>.bundle, takes the class named
 * MetalPluginClassName (a subclass of Metal's _MTLDevice) and calls
 * -initWithAcceleratorPort: with the service. SweetAccelerator publishes
 * itself under that class name with the properties that point Metal at
 * SweetMetal.bundle (phone/metal), whose device drives the Adreno 640
 * through bsd/dev/sweet_gpu.c.
 *
 * Boot-arg sweetmtl=1 publishes it; without it Metal finds no device and
 * Core Animation keeps its software renderer.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class SweetAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(SweetAccelerator);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **)
	    APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAccelerator, IOAcceleratorES);

/* Published plugin name, and the instance the overlay redirects it through
 * when it could only install the bundle under /private/var/local. */
static SweetAccelerator *g_accel;
static char g_plugin[96] = "SweetMetal";

IOService *
SweetAccelerator::probe(IOService *provider, SInt32 *score)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sweetmtl", &on, sizeof(on));
	return on ? IOService::probe(provider, score) : NULL;
}

bool
SweetAccelerator::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	/* Metal builds "/System/Library/Extensions/<name>.bundle" from this, so the
	 * name is also the way out of a read-only system directory: sweetmtlname=
	 * ../../../private/var/local/SweetMetal normalises to a path the overlay can
	 * always write. Default stays the plain bundle name. */
	char plugin[96];
	if (!PE_parse_boot_argn("sweetmtlname", plugin, sizeof(plugin)) || plugin[0] == 0) {
		strlcpy(plugin, "SweetMetal", sizeof(plugin));
	}
	setProperty("MetalPluginName", plugin);
	setProperty("MetalPluginClassName", "SweetMTLDevice");
	setProperty("IOGLBundleName", plugin);
	setProperty("model", "Adreno 640");
	registerService();
	IOLog("SWEET MTL: IOAcceleratorES published (MetalPluginName %s)\n", plugin);
	g_accel = this;
	{
		uint32_t h = 0;
		if (PE_parse_boot_argn("sweetmtl_halt", &h, sizeof(h)) && h) {
			panic("SWEET MTL: IOAcceleratorES published (sweetmtl_halt=1)");
		}
	}
	return true;
}

/* bsd/dev/sweet_overlay.c calls this when it installed the bundle only into
 * /private/var/local: Metal builds the path from these properties, so pointing
 * the name at "../../../private/var/local/SweetMetal" makes it load the copy
 * that exists instead of the one the read-only /System refused. */
extern "C" void
sweet_metal_plugin_set(const char *name)
{
	if (name == NULL || name[0] == 0 || g_accel == NULL) {
		return;
	}
	strlcpy(g_plugin, name, sizeof(g_plugin));
	g_accel->setProperty("MetalPluginName", g_plugin);
	g_accel->setProperty("IOGLBundleName", g_plugin);
	IOLog("SWEET MTL: MetalPluginName redirected to %s\n", g_plugin);
}

/* The user client lives in bsd/dev/sweet_accel_client.cpp. */
extern "C" IOService *sweet_accel_create_user_client(IOService *provider);

IOReturn
SweetAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	(void)owningTask; (void)securityID; (void)type; (void)properties;
	if (handler == NULL) return kIOReturnBadArgument;
	IOService *client = sweet_accel_create_user_client(this);
	if (client == NULL) return kIOReturnNoMemory;
	IOUserClient *uc = OSDynamicCast(IOUserClient, client);
	if (uc == NULL) { client->release(); return kIOReturnError; }
	*handler = uc;
	return kIOReturnSuccess;
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
