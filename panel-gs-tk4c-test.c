/* SPDX-License-Identifier: MIT */

#include "gs_panel/gs_panel_test.h"

/* Registers */

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };

const struct gs_dsi_cmd irc_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0x03, 0x68),
};
static DEFINE_GS_CMDSET(irc_read);

const struct gs_dsi_cmd fgz_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0x61, 0x68),
};
static DEFINE_GS_CMDSET(fgz_read);

const struct gs_dsi_cmd opr_read_cmds[] = {
	GS_DSI_CMD(0xB0, 0x00, 0xD8, 0x63),
};
static DEFINE_GS_CMDSET(opr_read);

const struct gs_dsi_cmd test_key_enable_cmds[] = {
	GS_DSI_CMDLIST(test_key_enable),
};
static DEFINE_GS_CMDSET(test_key_enable);

const struct gs_dsi_cmd test_key_disable_cmds[] = {
	GS_DSI_CMDLIST(test_key_disable),
};
static DEFINE_GS_CMDSET(test_key_disable);

/* sorted by register address */
static const struct gs_panel_register tk4c_registers[] = {
	GS_PANEL_REG("lp_status", 0x54),
	GS_PANEL_REG_LONG_WITH_CMDS("opr", 0x63, 2, &opr_read_cmdset),
	GS_PANEL_REG_WITH_CMDS("irc", 0x68, &irc_read_cmdset),
	GS_PANEL_REG_LONG_WITH_CMDS("fgz", 0x68, 8, &fgz_read_cmdset),
	GS_PANEL_REG_LONG("refresh_rate", 0x84, 2),
};

static const struct gs_panel_registers_desc tk4c_reg_desc = {
	.register_count = ARRAY_SIZE(tk4c_registers),
	.registers = tk4c_registers,

	.global_pre_read_cmdset = &test_key_enable_cmdset,
	.global_post_read_cmdset = &test_key_disable_cmdset,
};

/* Query functions */

static bool array_is_equal(const u8 *l, const u8 *r, int count)
{
	return memcmp(l, r, count * sizeof(u8)) == 0;
}

struct array_to_value {
	const u8 *array;
	const int value;
	const u64 rev;
};

struct gs_panel_register_query {
	const struct gs_panel_register *reg;
	const struct array_to_value *map;
	int map_size;
	int default_result;
};

int get_query_result_from_register(struct gs_panel_test *test,
				   const struct gs_panel_register_query *query)
{
	struct gs_panel *ctx = test->ctx;
	int i, ret = -1;
	u8 *read_result;

	if (!query || !query->reg)
		return ret;

	read_result = kmalloc_array(query->reg->size, sizeof(u8), GFP_KERNEL);

	if (gs_panel_read_register_value(test, query->reg, read_result))
		goto free_mem;

	ret = query->default_result;
	for (i = 0; i < query->map_size; i++) {
		if (query->map[i].rev && !(ctx->panel_rev & query->map[i].rev))
			continue;

		if (array_is_equal(read_result, query->map[i].array, query->reg->size)) {
			ret = query->map[i].value;
			goto free_mem;
		}
	}

free_mem:
	kfree(read_result);
	return ret;
}

int tk4c_query_is_aod_on(struct gs_panel_test *test)
{
	static const u8 aod_on_value[] = { 0x24 };

	static struct array_to_value aod_read_map[] = {
		{ .array = aod_on_value, .value = 1 },
	};

	const struct gs_panel_register_query aod_query = {
		.reg = &tk4c_registers[0],
		.map = aod_read_map,
		.map_size = ARRAY_SIZE(aod_read_map),
		.default_result = 0,
	};

	return get_query_result_from_register(test, &aod_query);
}

int tk4c_query_get_refresh_rate(struct gs_panel_test *test)
{
	static const u8 rr_120[] = { 0x00, 0x00 };
	static const u8 rr_60[] = { 0x08, 0x00 };

	static const struct array_to_value rr_read_map[] = {
		{ .array = rr_120, .value = 120 },
		{ .array = rr_60, .value = 60 },
	};

	static const struct gs_panel_register_query refresh_rate_query = {
		.reg = &tk4c_registers[4],
		.map = rr_read_map,
		.map_size = ARRAY_SIZE(rr_read_map),
		.default_result = 0,
	};

	if (tk4c_query_is_aod_on(test) == 1)
		return 30;

	return get_query_result_from_register(test, &refresh_rate_query);
}

int tk4c_query_is_irc_on(struct gs_panel_test *test)
{
	static const u8 irc_on[] = { 0x25 };
	static const u8 ifc_off[] = { 0x05 };

	static const struct array_to_value irc_read_map[] = {
		{ .array = irc_on, .value = 1 },
		{ .array = ifc_off, .value = 0 },
	};

	static const struct gs_panel_register_query irc_query = {
		.reg = &tk4c_registers[2],
		.map = irc_read_map,
		.map_size = ARRAY_SIZE(irc_read_map),
		.default_result = -1,
	};

	return get_query_result_from_register(test, &irc_query);
}

const struct gs_panel_query_funcs tk4c_gs_query_func = {
	.get_refresh_rate = tk4c_query_get_refresh_rate,
	.get_irc_on = tk4c_query_is_irc_on,
	.get_aod_on = tk4c_query_is_aod_on,
};

/* Custom query functions */

static int tk4c_query_fgz_on(struct gs_panel_test *test)
{
	static u8 fgz_off_value[] = { 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0x00, 0x00 };
	static u8 fgz_on_value_EVT[] = { 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0xF5, 0xC4 };
	static u8 fgz_on_value_DVT[] = { 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0xE4, 0xB6 };
	static u8 fgz_on_value_PVT[] = { 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0xE4, 0xB6 };

	static const struct array_to_value fgz_read_map[] = {
		{ .array = fgz_off_value, .value = 0 },
		{ .array = fgz_on_value_EVT, .value = 1, .rev = PANEL_REV_LT(PANEL_REV_DVT1) },
		{ .array = fgz_on_value_DVT,
		  .value = 1,
		  .rev = (PANEL_REV_DVT1 | PANEL_REV_DVT1_1) },
		{ .array = fgz_on_value_PVT, .value = 1, .rev = PANEL_REV_GE(PANEL_REV_PVT) },
	};

	static const struct gs_panel_register_query fgz_query = {
		.reg = &tk4c_registers[3],
		.map = fgz_read_map,
		.map_size = ARRAY_SIZE(fgz_read_map),
		.default_result = -1,
	};

	return get_query_result_from_register(test, &fgz_query);
}

static int tk4c_fgz_show(struct seq_file *m, void *data)
{
	struct gs_panel_test *test = m->private;

	if (!test)
		return -EFAULT;

	seq_printf(m, "%d\n", tk4c_query_fgz_on(test));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tk4c_fgz);

int tk4c_add_custom_query_nodes(struct gs_panel_test *test, struct dentry *test_root)
{
	struct dentry *query_root;

	query_root = debugfs_lookup("query", test_root);
	if (!query_root)
		return -EFAULT;

	debugfs_create_file("fgz_on", 0600, query_root, test, &tk4c_fgz_fops);

	/* TODO: add OPR */

	return 0;
}

/**
 * struct tk4c_panel_test - panel specific test runtime info
 *
 * Only maintains tk4c panel test specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_test_desc
 */
struct tk4c_panel_test {
	/** @base: base panel struct */
	struct gs_panel_test base;

	/* add panel specific test data here */
};
#define to_spanel_test(test) container_of(test, struct tk4c_panel_test, base)

static void tk4c_test_debugfs_init(struct gs_panel_test *test, struct dentry *test_root)
{
	tk4c_add_custom_query_nodes(test, test_root);
}

static const struct gs_panel_test_funcs tk4c_test_func = {
	.debugfs_init = tk4c_test_debugfs_init,
};

static const struct gs_panel_test_desc google_tk4c_test = {
	.test_funcs = &tk4c_test_func,
	.regs_desc = &tk4c_reg_desc,
	.query_desc = &tk4c_gs_query_func,
};

static int tk4c_panel_test_probe(struct platform_device *pdev)
{
	struct tk4c_panel_test *stest;

	stest = devm_kzalloc(&pdev->dev, sizeof(*stest), GFP_KERNEL);
	if (!stest)
		return -ENOMEM;

	return gs_panel_test_common_init(pdev, &stest->base);
}

static const struct of_device_id gs_panel_test_of_match[] = {
	{ .compatible = "google,gs-tk4c-test", .data = &google_tk4c_test },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_test_of_match);

static struct platform_driver gs_panel_test_driver = {
	.probe = tk4c_panel_test_probe,
	.remove = gs_panel_test_common_remove,
	.driver = {
		.name = "gs-tk4c-test",
		.of_match_table = gs_panel_test_of_match,
	},
};
module_platform_driver(gs_panel_test_driver);

MODULE_AUTHOR("Safayat Ullah <safayat@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4c panel test driver");
MODULE_LICENSE("Dual MIT/GPL");
