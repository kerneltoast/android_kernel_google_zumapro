// SPDX-License-Identifier: GPL-2.0
/*
 * Common platform interfaces for mobile TPU chips.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <gcip/gcip-pm.h>
#include <gcip/gcip-iommu.h>
#include <gcip/iif/iif-manager.h>

#include "edgetpu-config.h"
#include "edgetpu-debug-dump.h"
#include "edgetpu-dmabuf.h"
#include "edgetpu-gsa.h"
#include "edgetpu-internal.h"
#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"
#include "edgetpu-mobile-platform.h"
#include "edgetpu-soc.h"
#include "edgetpu-telemetry.h"
#include "edgetpu-thermal.h"
#include "mobile-firmware.h"

static struct edgetpu_dev *edgetpu_debug_pointer;

static void set_telemetry_mem(struct edgetpu_mobile_platform_dev *etmdev,
			      enum gcip_telemetry_type type, struct edgetpu_coherent_mem *mem)
{
	int i, offset = type == GCIP_TELEMETRY_TRACE ? EDGETPU_TELEMETRY_LOG_BUFFER_SIZE : 0;
	const size_t size = type == GCIP_TELEMETRY_LOG ? EDGETPU_TELEMETRY_LOG_BUFFER_SIZE :
							 EDGETPU_TELEMETRY_TRACE_BUFFER_SIZE;

	for (i = 0; i < etmdev->edgetpu_dev.num_cores; i++) {
		mem[i].vaddr = etmdev->shared_mem_vaddr + offset;
		mem[i].dma_addr = etmdev->remapped_data_addr + offset;
		mem[i].host_addr = 0;
		mem[i].size = size;
		offset += EDGETPU_TELEMETRY_LOG_BUFFER_SIZE + EDGETPU_TELEMETRY_TRACE_BUFFER_SIZE;
	}
}

void edgetpu_mobile_set_telemetry_mem(struct edgetpu_mobile_platform_dev *etmdev)
{
	set_telemetry_mem(etmdev, GCIP_TELEMETRY_LOG, etmdev->log_mem);
	set_telemetry_mem(etmdev, GCIP_TELEMETRY_TRACE, etmdev->trace_mem);
}

static int edgetpu_platform_setup_fw_region(struct edgetpu_mobile_platform_dev *etmdev)
{
	struct edgetpu_dev *etdev = &etmdev->edgetpu_dev;
	struct platform_device *gsa_pdev;
	struct device *dev = etdev->dev;
	struct resource r;
	struct device_node *np;
	int err;
	size_t region_map_size = EDGETPU_MAX_FW_LIMIT;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(dev, "No memory region for firmware");
		return -ENODEV;
	}

	err = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (err) {
		dev_err(dev, "No memory address assigned to firmware region");
		return err;
	}

	if (resource_size(&r) < region_map_size) {
		dev_err(dev, "Memory region for firmware too small (%zu bytes needed, got %llu)",
			region_map_size, resource_size(&r));
		return -ENOSPC;
	}

	/* Get GSA device from device tree */
	np = of_parse_phandle(dev->of_node, "gsa-device", 0);
	if (!np) {
		dev_warn(dev, "No gsa-device in device tree. Authentication not available");
	} else {
		gsa_pdev = of_find_device_by_node(np);
		if (!gsa_pdev) {
			dev_err(dev, "GSA device not found");
			of_node_put(np);
			return -ENODEV;
		}
		etmdev->gsa_dev = get_device(&gsa_pdev->dev);
		of_node_put(np);
	}

	etmdev->fw_region_paddr = r.start;
	etmdev->fw_region_size = EDGETPU_DEFAULT_FW_LIMIT;

	etmdev->remapped_data_addr = EDGETPU_INSTRUCTION_REMAP_BASE + etmdev->fw_region_size;
	etmdev->remapped_data_size = EDGETPU_DEFAULT_REMAPPED_DATA_SIZE;

	etmdev->shared_mem_vaddr = memremap(etmdev->fw_region_paddr + etmdev->fw_region_size,
					    etmdev->remapped_data_size, MEMREMAP_WC);
	if (!etmdev->shared_mem_vaddr) {
		dev_err(dev, "Shared memory remap failed");
		if (etmdev->gsa_dev)
			put_device(etmdev->gsa_dev);
		return -EINVAL;
	}
	etmdev->shared_mem_paddr = etmdev->fw_region_paddr + etmdev->fw_region_size;

	return 0;
}

static void edgetpu_platform_cleanup_fw_region(struct edgetpu_mobile_platform_dev *etmdev)
{
	if (etmdev->gsa_dev) {
		gsa_unload_tpu_fw_image(etmdev->gsa_dev);
		put_device(etmdev->gsa_dev);
	}
	if (!etmdev->shared_mem_vaddr)
		return;
	memunmap(etmdev->shared_mem_vaddr);
	etmdev->shared_mem_vaddr = NULL;
	etmdev->remapped_data_addr = 0;
	etmdev->remapped_data_size = 0;
}

static int mobile_check_ext_mailbox_args(const char *func, struct edgetpu_dev *etdev,
					 struct edgetpu_ext_mailbox_ioctl *args)
{
	if (args->type != EDGETPU_EXT_MAILBOX_TYPE_TZ) {
		etdev_err(etdev, "%s: Invalid type %d != %d\n", func, args->type,
			  EDGETPU_EXT_MAILBOX_TYPE_TZ);
		return -EINVAL;
	}
	if (args->count != 1) {
		etdev_err(etdev, "%s: Invalid mailbox count: %d != 1\n", func, args->count);
		return -EINVAL;
	}
	return 0;
}

int edgetpu_chip_acquire_ext_mailbox(struct edgetpu_client *client,
				     struct edgetpu_ext_mailbox_ioctl *args)
{
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(client->etdev);
	int ret;

	ret = mobile_check_ext_mailbox_args(__func__, client->etdev, args);
	if (ret)
		return ret;

	mutex_lock(&etmdev->tz_mailbox_lock);
	if (etmdev->secure_client) {
		etdev_err(client->etdev, "TZ mailbox already in use by PID %d\n",
			  etmdev->secure_client->pid);
		mutex_unlock(&etmdev->tz_mailbox_lock);
		return -EBUSY;
	}
	ret = edgetpu_mailbox_enable_ext(client, EDGETPU_TZ_MAILBOX_ID, NULL, 0);
	if (!ret)
		etmdev->secure_client = client;
	mutex_unlock(&etmdev->tz_mailbox_lock);
	return ret;
}

int edgetpu_chip_release_ext_mailbox(struct edgetpu_client *client,
				     struct edgetpu_ext_mailbox_ioctl *args)
{
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(client->etdev);
	int ret = 0;

	ret = mobile_check_ext_mailbox_args(__func__, client->etdev,
					      args);
	if (ret)
		return ret;

	mutex_lock(&etmdev->tz_mailbox_lock);
	if (!etmdev->secure_client) {
		etdev_warn(client->etdev, "TZ mailbox already released\n");
		mutex_unlock(&etmdev->tz_mailbox_lock);
		return 0;
	}
	if (etmdev->secure_client != client) {
		etdev_err(client->etdev,
			  "TZ mailbox owned by different client\n");
		mutex_unlock(&etmdev->tz_mailbox_lock);
		return -EBUSY;
	}
	etmdev->secure_client = NULL;
	ret = edgetpu_mailbox_disable_ext(client, EDGETPU_TZ_MAILBOX_ID);
	mutex_unlock(&etmdev->tz_mailbox_lock);
	return ret;
}

void edgetpu_chip_client_remove(struct edgetpu_client *client)
{
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(client->etdev);

	mutex_lock(&etmdev->tz_mailbox_lock);
	if (etmdev->secure_client == client) {
		etmdev->secure_client = NULL;
		edgetpu_mailbox_disable_ext(client, EDGETPU_TZ_MAILBOX_ID);
	}
	mutex_unlock(&etmdev->tz_mailbox_lock);
}

/* Handle mailbox response doorbell IRQ for mobile platform devices. */
static irqreturn_t edgetpu_platform_handle_mailbox_doorbell(struct edgetpu_dev *etdev, int irq)
{
	struct edgetpu_mailbox *mailbox;
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(etdev);
	struct edgetpu_mailbox_manager *mgr = etdev->mailbox_manager;
	uint i;

	if (!mgr)
		return IRQ_NONE;
	for (i = 0; i < etmdev->n_mailbox_irq; i++)
		if (etmdev->mailbox_irq[i] == irq)
			break;
	if (i == etmdev->n_mailbox_irq)
		return IRQ_NONE;
	read_lock(&mgr->mailboxes_lock);
	mailbox = mgr->mailboxes[i];
	if (!mailbox)
		goto out;
	if (!EDGETPU_MAILBOX_RESP_QUEUE_READ(mailbox, doorbell_status))
		goto out;
	EDGETPU_MAILBOX_RESP_QUEUE_WRITE(mailbox, doorbell_clear, 1);
	etdev_dbg(mgr->etdev, "mbox %u resp doorbell irq tail=%u\n", i,
		  EDGETPU_MAILBOX_RESP_QUEUE_READ(mailbox, tail));
	if (mailbox->handle_irq)
		mailbox->handle_irq(mailbox);
out:
	read_unlock(&mgr->mailboxes_lock);
	return IRQ_HANDLED;
}

/* Handle a mailbox response doorbell interrupt. */
irqreturn_t edgetpu_mailbox_irq_handler(int irq, void *arg)
{
	struct edgetpu_dev *etdev = arg;

	edgetpu_telemetry_irq_handler(etdev);
	edgetpu_debug_dump_resp_handler(etdev);
	return edgetpu_platform_handle_mailbox_doorbell(etdev, irq);
}

static inline const char *get_driver_commit(void)
{
#if IS_ENABLED(CONFIG_MODULE_SCMVERSION)
	return THIS_MODULE->scmversion ?: "scmversion missing";
#elif defined(GIT_REPO_TAG)
	return GIT_REPO_TAG;
#else
	return "Unknown";
#endif
}

static int edgetpu_mobile_platform_probe(struct platform_device *pdev,
					 struct edgetpu_mobile_platform_dev *etmdev)
{
	struct device *dev = &pdev->dev;
	struct edgetpu_dev *etdev = &etmdev->edgetpu_dev;
	struct resource *r;
	struct edgetpu_mapped_resource regs;
	int ret;
	struct edgetpu_iface_params iface_params[] = {
		/* Default interface  */
		{ .name = NULL },
		/* Common name for embedded SoC devices */
		{ .name = "edgetpu-soc" },
	};

	mutex_init(&etmdev->tz_mailbox_lock);

	platform_set_drvdata(pdev, etdev);
	etdev->dev = dev;
	etdev->num_cores = EDGETPU_NUM_CORES;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR_OR_NULL(r)) {
		dev_err(dev, "failed to get memory resource");
		return -ENODEV;
	}

	regs.phys = r->start;
	regs.size = resource_size(r);
	regs.mem = devm_ioremap_resource(dev, r);
	if (IS_ERR(regs.mem)) {
		ret = PTR_ERR(regs.mem);
		dev_err(dev, "failed to map TPU TOP registers: %d", ret);
		return ret;
	}

	mutex_init(&etmdev->platform_pwr.policy_lock);
	etmdev->platform_pwr.curr_policy = TPU_POLICY_MAX;

	/* Use 36-bit DMA mask for any default DMA API paths except coherent. */
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(36));
	if (ret)
		dev_warn(dev, "dma_set_mask returned %d\n", ret);
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(dev, "dma_set_coherent_mask returned %d\n", ret);

	ret = edgetpu_platform_setup_fw_region(etmdev);
	if (ret) {
		dev_err(dev, "setup fw regions failed: %d", ret);
		goto out_shutdown;
	}

	ret = edgetpu_iremap_pool_create(etdev,
					 /* Base virtual address (kernel address space) */
					 etmdev->shared_mem_vaddr + EDGETPU_POOL_MEM_OFFSET,
					 /* Base DMA address */
					 etmdev->remapped_data_addr + EDGETPU_POOL_MEM_OFFSET,
					 /* Base physical address */
					 etmdev->shared_mem_paddr + EDGETPU_POOL_MEM_OFFSET,
					 /* Size */
					 etmdev->remapped_data_size - EDGETPU_POOL_MEM_OFFSET,
					 /* Granularity */
					 PAGE_SIZE);
	if (ret) {
		dev_err(dev, "failed to initialize remapped memory pool: %d", ret);
		goto out_cleanup_fw;
	}

	INIT_LIST_HEAD(&etmdev->fw_ctx_list);
	mutex_init(&etmdev->fw_ctx_list_lock);

	ret = edgetpu_device_add(etdev, &regs, iface_params, ARRAY_SIZE(iface_params));
	if (ret) {
		dev_err(dev, "edgetpu setup failed: %d", ret);
		goto out_destroy_iremap;
	}

	ret = edgetpu_soc_setup_irqs(etdev);
	if (ret) {
		dev_err(dev, "IRQ setup failed: %d", ret);
		goto out_remove_device;
	}

	etmdev->log_mem = devm_kcalloc(dev, etdev->num_cores, sizeof(*etmdev->log_mem), GFP_KERNEL);
	if (!etmdev->log_mem) {
		ret = -ENOMEM;
		goto out_remove_device;
	}

#if IS_ENABLED(CONFIG_EDGETPU_TELEMETRY_TRACE)
	etmdev->trace_mem =
		devm_kcalloc(dev, etdev->num_cores, sizeof(*etmdev->log_mem), GFP_KERNEL);
	if (!etmdev->trace_mem) {
		ret = -ENOMEM;
		goto out_remove_device;
	}
#endif

	edgetpu_mobile_set_telemetry_mem(etmdev);
	ret = edgetpu_telemetry_init(etdev, etmdev->log_mem, etmdev->trace_mem);
	if (ret)
		goto out_remove_device;

	ret = edgetpu_mobile_firmware_create(etdev);
	if (ret) {
		dev_err(dev, "initialize firmware downloader failed: %d", ret);
		goto out_tel_exit;
	}

	ret = edgetpu_thermal_create(etdev);
	if (ret)
		etdev_warn(etdev, "Failed to create thermal device: %d", ret);

	ret = edgetpu_sync_fence_manager_create(etdev);
	if (ret) {
		etdev_err(etdev, "Failed to create DMA fence manager: %d", ret);
		goto out_destroy_thermal;
	}

	etdev->iif_mgr = iif_manager_init(etdev->dev->of_node);
	if (IS_ERR(etdev->iif_mgr)) {
		etdev_warn(etdev, "Failed to init IIF manager: %ld", PTR_ERR(etdev->iif_mgr));
		etdev->iif_mgr = NULL;
	}

	if (etmdev->after_probe) {
		ret = etmdev->after_probe(etmdev);
		if (ret) {
			dev_err(dev, "after_probe callback failed: %d", ret);
			goto out_put_iif_mgr;
		}
	}

	dev_info(dev, "%s edgetpu initialized. Build: %s", etdev->dev_name, get_driver_commit());

	/* Turn the device off unless a client request is already received. */
	gcip_pm_shutdown(etdev->pm, false);

	edgetpu_debug_pointer = etdev;

	return 0;

out_put_iif_mgr:
	if (etdev->iif_mgr)
		iif_manager_put(etdev->iif_mgr);
out_destroy_thermal:
	edgetpu_thermal_destroy(etdev);
	edgetpu_mobile_firmware_destroy(etdev);
out_tel_exit:
	edgetpu_telemetry_exit(etdev);
out_remove_device:
	edgetpu_device_remove(etdev);
out_destroy_iremap:
	edgetpu_iremap_pool_destroy(etdev);
out_cleanup_fw:
	edgetpu_platform_cleanup_fw_region(etmdev);
out_shutdown:
	dev_dbg(dev, "Probe finished with error %d, powering down", ret);
	gcip_pm_shutdown(etdev->pm, true);
	return ret;
}

static int edgetpu_mobile_platform_remove(struct platform_device *pdev)
{
	struct edgetpu_dev *etdev = platform_get_drvdata(pdev);
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(etdev);

	if (etdev->iif_mgr)
		iif_manager_put(etdev->iif_mgr);
	edgetpu_thermal_destroy(etdev);
	edgetpu_mobile_firmware_destroy(etdev);
	edgetpu_telemetry_exit(etdev);
	edgetpu_device_remove(etdev);
	edgetpu_iremap_pool_destroy(etdev);
	edgetpu_platform_cleanup_fw_region(etmdev);

	edgetpu_debug_pointer = NULL;

	return 0;
}
