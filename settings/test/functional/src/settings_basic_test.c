/*
 * Copyright (c) 2018 Laczen
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 *  @brief Settings functional test suite
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
/* LOG_MODULE_REGISTER(settings_basic_test); */
LOG_MODULE_REGISTER(settings_basic_test, CONFIG_SETTINGS_LOG_LEVEL);

#if defined(CONFIG_SETTINGS_FCB) || defined(CONFIG_SETTINGS_NVS) || defined(CONFIG_SETTINGS_ZMS)
#include <zephyr/storage/flash_map.h>
#if DT_HAS_CHOSEN(zephyr_settings_partition)
#define TEST_FLASH_AREA_ID DT_FIXED_PARTITION_ID(DT_CHOSEN(zephyr_settings_partition))
#endif
#elif defined(CONFIG_SETTINGS_FILE)
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#else
#error "Settings backend not selected"
#endif

#ifndef TEST_FLASH_AREA_ID
#define TEST_FLASH_AREA		storage_partition
#define TEST_FLASH_AREA_ID	FIXED_PARTITION_ID(TEST_FLASH_AREA)
#endif

/* The standard test expects a cleared flash area.  Make sure it has
 * one.
 */
ZTEST(settings_functional, test_clear_settings)
{
#if !defined(CONFIG_SETTINGS_FILE)
	const struct flash_area *fap;
	int rc;

	rc = flash_area_open(TEST_FLASH_AREA_ID, &fap);

	if (rc == 0) {
		rc = flash_area_flatten(fap, 0, fap->fa_size);
		flash_area_close(fap);
	}
	zassert_true(rc == 0, "clear settings failed");
#else
	FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);

	/* mounting info */
	static struct fs_mount_t littlefs_mnt = {
		.type = FS_LITTLEFS,
		.fs_data = &cstorage,
		.storage_dev = (void *)TEST_FLASH_AREA_ID,
		.mnt_point = "/ff"
	};

	int rc;

	rc = fs_mount(&littlefs_mnt);
	zassert_true(rc == 0, "mounting littlefs [%d]\n", rc);

	rc = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
	zassert_true(rc == 0 || rc == -ENOENT,
		     "can't delete config file%d\n", rc);
#endif
}

/*
 * Test the two support routines that settings provides:
 *
 *   settings_name_steq(name, key, next): compares the start of name with key
 *   settings_name_next(name, next): returns the location of the first
 *                                   separator
 */

ZTEST(settings_functional, test_support_rtn)
{
	const char test1[] = "bt/a/b/c/d";
	const char test2[] = "bt/a/b/c/d=";
	const char *next1, *next2;
	int rc;

	/* complete match: return 1, next = NULL */
	rc = settings_name_steq(test1, "bt/a/b/c/d", &next1);
	zassert_true(rc == 1, "_steq comparison failure");
	zassert_is_null(next1, "_steq comparison next error");
	rc = settings_name_steq(test2, "bt/a/b/c/d", &next2);
	zassert_true(rc == 1, "_steq comparison failure");
	zassert_is_null(next2, "_steq comparison next error");

	/* partial match: return 1, next <> NULL */
	rc = settings_name_steq(test1, "bt/a/b/c", &next1);
	zassert_true(rc == 1, "_steq comparison failure");
	zassert_not_null(next1, "_steq comparison next error");
	zassert_equal_ptr(next1, test1+9, "next points to wrong location");
	rc = settings_name_steq(test2, "bt/a/b/c", &next2);
	zassert_true(rc == 1, "_steq comparison failure");
	zassert_not_null(next2, "_steq comparison next error");
	zassert_equal_ptr(next2, test2+9, "next points to wrong location");

	/* no match: return 0, next = NULL */
	rc = settings_name_steq(test1, "bta", &next1);
	zassert_true(rc == 0, "_steq comparison failure");
	zassert_is_null(next1, "_steq comparison next error");
	rc = settings_name_steq(test2, "bta", &next2);
	zassert_true(rc == 0, "_steq comparison failure");
	zassert_is_null(next2, "_steq comparison next error");

	/* no match: return 0, next = NULL */
	rc = settings_name_steq(test1, "b", &next1);
	zassert_true(rc == 0, "_steq comparison failure");
	zassert_is_null(next1, "_steq comparison next error");
	rc = settings_name_steq(test2, "b", &next2);
	zassert_true(rc == 0, "_steq comparison failure");
	zassert_is_null(next2, "_steq comparison next error");

	/* first separator: return 2, next <> NULL */
	rc = settings_name_next(test1, &next1);
	zassert_true(rc == 2, "_next wrong return value");
	zassert_not_null(next1, "_next wrong next");
	zassert_equal_ptr(next1, test1+3, "next points to wrong location");
	rc = settings_name_next(test2, &next2);
	zassert_true(rc == 2, "_next wrong return value");
	zassert_not_null(next2, "_next wrong next");
	zassert_equal_ptr(next2, test2+3, "next points to wrong location");

	/* second separator: return 1, next <> NULL */
	rc = settings_name_next(next1, &next1);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next1, "_next wrong next");
	zassert_equal_ptr(next1, test1+5, "next points to wrong location");
	rc = settings_name_next(next2, &next2);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next2, "_next wrong next");
	zassert_equal_ptr(next2, test2+5, "next points to wrong location");

	/* third separator: return 1, next <> NULL */
	rc = settings_name_next(next1, &next1);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next1, "_next wrong next");
	rc = settings_name_next(next2, &next2);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next2, "_next wrong next");

	/* fourth separator: return 1, next <> NULL */
	rc = settings_name_next(next1, &next1);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next1, "_next wrong next");
	rc = settings_name_next(next2, &next2);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_not_null(next2, "_next wrong next");

	/* fifth separator: return 1, next == NULL */
	rc = settings_name_next(next1, &next1);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_is_null(next1, "_next wrong next");
	rc = settings_name_next(next2, &next2);
	zassert_true(rc == 1, "_next wrong return value");
	zassert_is_null(next2, "_next wrong next");

}

struct stored_data {
	uint8_t val1;
	uint8_t val2;
	uint8_t val3;
	bool en1;
	bool en2;
	bool en3;
};

struct stored_data data;

int val1_set(const char *key, size_t len, settings_read_cb read_cb,
	     void *cb_arg)
{
	LOG_DBG("key:%s", key);
	data.val1 = 1;
	return 0;
}
int val1_commit(void)
{
	LOG_DBG("");
	data.en1 = true;
	return 0;
}
static struct settings_handler val1_settings = {
	.name = "ps",
	.h_set = val1_set,
	.h_commit = val1_commit,
};

int val2_set(const char *key, size_t len, settings_read_cb read_cb,
	     void *cb_arg)
{
	LOG_DBG("key:%s", key);
	data.val2 = 2;
	return 0;
}
int val2_commit(void)
{
	LOG_DBG("");
	data.en2 = true;
	return 0;
}
static struct settings_handler val2_settings = {
	.name = "ps/ss/ss",
	.h_set = val2_set,
	.h_commit = val2_commit,
};

int val3_set(const char *key, size_t len, settings_read_cb read_cb,
	     void *cb_arg)
{
	LOG_DBG("key:%s", key);
	data.val3 = 3;
	return 0;
}
int val3_commit(void)
{
	LOG_DBG("");
	data.en3 = true;
	return 0;
}
static struct settings_handler val3_settings = {
	.name = "ps/ss",
	.h_set = val3_set,
	.h_commit = val3_commit,
};

/* settings --      name
 * val1_settings    ps
 * val2_settings    ps/ss/ss
 * val3_settings    ps/ss
 */

/* helper routine to remove a handler from settings */
int settings_deregister(struct settings_handler *handler)
{
	extern sys_slist_t settings_handlers;

	return sys_slist_find_and_remove(&settings_handlers, &handler->node);
}

ZTEST(settings_functional, test_register_and_loading)
{
	int rc, err;
	uint8_t val = 0;
	ssize_t val_len = 0;

	rc = settings_subsys_init();
	zassert_true(rc == 0, "subsys init failed");


	/* Check that key that corresponds to val2 do not exist in storage */
	val_len = settings_get_val_len("ps/ss/ss/val2");
	zassert_true((val_len == 0), "Failure: key should not exist");

	settings_save_one("ps/ss/ss/val2", &val, sizeof(uint8_t));

	/* Check that the key that corresponds to val2 exists in storage */
	val_len = settings_get_val_len("ps/ss/ss/val2");
	zassert_true((val_len == 1), "Failure: key should exist");

	memset(&data, 0, sizeof(struct stored_data));

	rc = settings_register(&val1_settings);
	zassert_true(rc == 0, "register of val1 settings failed");

	/* when we load settings now data.val1 should receive the value*/
	/* 因为 val1_settings 的 h_set() 中，设置的是 val1 */
	rc = settings_load();
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 1) && (data.val2 == 0) && (data.val3 == 0);
	zassert_true(err, "wrong data value found");
	/* commit is only called for val1_settings */
	err = (data.en1) && (!data.en2) && (!data.en3);
	zassert_true(err, "wrong data enable found");

	/* Next register should be ok */
	rc = settings_register(&val2_settings);
	zassert_true(rc == 0, "register of val2 settings failed");

	/* Next register should fail (same name) */
	rc = settings_register(&val2_settings);
	zassert_true(rc == -EEXIST, "double register of val2 settings allowed");


/*
 * settings --      name
 * val1_settings    ps          registered
 * val2_settings    ps/ss/ss    registered
 * val3_settings    ps/ss       not registered
 *
 * 此时存储的数据有：
 *  data            h_set命中的register        h_set key_name
 * ps/ss/ss/val2    val2_settings               val2
 */
	memset(&data, 0, sizeof(struct stored_data));
	LOG_DBG("settings_load(). after val1&val2 registered");
	/* when we load settings now data.val2 should receive the value*/
/* TODO: 没有理解，为什么 data.val1 的值没有改变.
 * 分析结果：
 * settings_call_set_handler()->settings_parse_and_lookup() 中，
 * 根据给定的配置项名称，
 * 在静态和动态注册的settings处理器中查找最佳匹配项。
 * 当配置项名称包含多级路径时，
 * 函数能够识别出匹配路径最长的处理器并返回剩余路径部分。
 */
	rc = settings_load();
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 0) && (data.val2 == 2) && (data.val3 == 0);
	zassert_true(err, "wrong data value found");
	/* commit is called for val1_settings and val2_settings*/
	err = (data.en1) && (data.en2) && (!data.en3);
	zassert_true(err, "wrong data enable found");

	/* Check that key that corresponds to val3 do not exist in storage */
	val_len = settings_get_val_len("ps/ss/val3");
	zassert_true((val_len == 0), "Failure: key should not exist");

	settings_save_one("ps/ss/val3", &val, sizeof(uint8_t));

	/* Check that the key that corresponds to val3 exists in storage */
	val_len = settings_get_val_len("ps/ss/val3");
	zassert_true((val_len == 1), "Failure: key should exist");

	memset(&data, 0, sizeof(struct stored_data));

/*
 * settings --      name
 * val1_settings    ps          registered
 * val2_settings    ps/ss/ss    registered
 * val3_settings    ps/ss       not registered
 *
 * 此时存储的数据有：
 * data             h_set命中的register        h_set key_name
 * ps/ss/ss/val2    val2_settings               val2
 * ps/ss/val3       val1_settings               ss/val3
 */
/* when we load settings now data.val2 and data.val1 should receive a
 * value
 */
/* 此处，还没有 register val3_settings, 所以 data.val3 和 data.en3 的值没有改变
 * ps/ss/val3 匹配中 val1_settings(name=ps), val1_set回到中key=ss/val3.
 */
	LOG_DBG("settings_load(). after set val3=0");
	rc = settings_load();
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 1) && (data.val2 == 2) && (data.val3 == 0);
	zassert_true(err, "wrong data value found");
	/* commit is called for val1_settings and val2_settings*/
	err = (data.en1) && (data.en2) && (!data.en3);
	zassert_true(err, "wrong data enable found");

	/* val3 settings should be inserted in between val1_settings and
	 * val2_settings.
	 */
/* TODO: 为什么说 between ... ？ 这段逻辑在哪里？
 * int settings_register_with_cprio(...) 中
 * 只是将注册的 register 添加到现有列表的后面
 */
	rc = settings_register(&val3_settings);
	zassert_true(rc == 0, "register of val3 settings failed");
	memset(&data, 0, sizeof(struct stored_data));

	/* when we load settings now data.val2 and data.val3 should receive a
	 * value
	 */
/**
 * settings --      name
 * val1_settings    ps          registered
 * val2_settings    ps/ss/ss    registered
 * val3_settings    ps/ss       registered
 *
 * 此时存储的数据有：
 * data             h_set命中的register    h_set key_name
 * ps/ss/ss/val2    val2_settings           val2
 * ps/ss/val3       val3_settings           val3
 */
	LOG_DBG("settings_load(). after val1&val2/val3 registered");
	rc = settings_load();
	zassert_true(rc == 0, "settings_load failed");
	/* TODO: 为什么 data.val1 的值没有改变？ */
	err = (data.val1 == 0) && (data.val2 == 2) && (data.val3 == 3);
	zassert_true(err, "wrong data value found");
	/* commit is called for val1_settings, val2_settings and val3_settings
	 */
	err = (data.en1) && (data.en2) && (data.en3);
	zassert_true(err, "wrong data enable found");

	/* Check that key that corresponds to val1 do not exist in storage */
	val_len = settings_get_val_len("ps/val1");
	zassert_true((val_len == 0), "Failure: key should not exist");

	settings_save_one("ps/val1", &val, sizeof(uint8_t));

	/* Check that the key that corresponds to val1 exists in storage */
	val_len = settings_get_val_len("ps/val1");
	zassert_true((val_len == 1), "Failure: key should exist");

	memset(&data, 0, sizeof(struct stored_data));

/*
 * settings --  name
 * val1_settings    ps          registered
 * val2_settings    ps/ss/ss    registered
 * val3_settings    ps/ss       registered
 *
 * 此时存储的数据有：
 * data             h_set命中的register    h_set key_name
 * ps/ss/ss/val2    val2_settings           val2
 * ps/ss/val3       val3_settings           val3
 * ps/val1          val1_settings           val1
 */
	/* when we load settings all data should receive a value loaded */
	rc = settings_load();
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 1) && (data.val2 == 2) && (data.val3 == 3);
	zassert_true(err, "wrong data value found");
	/* commit is called for all settings*/
	err = (data.en1) && (data.en2) && (data.en3);
	zassert_true(err, "wrong data enable found");


	memset(&data, 0, sizeof(struct stored_data));
	/* test subtree loading: subtree "ps/ss" data.val2 and data.val3 should
	 * receive a value
	 */
	/*
	 * ps/ss/val3 和 ps/ss/ss/val2 数据都属于 subtree ps/ss
	 */
	rc = settings_load_subtree("ps/ss");
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 0) && (data.val2 == 2) && (data.val3 == 3);
	zassert_true(err, "wrong data value found");
	/* commit is called for val2_settings and val3_settings */
	err = (!data.en1) && (data.en2) && (data.en3);
	zassert_true(err, "wrong data enable found");

	memset(&data, 0, sizeof(struct stored_data));
	/* test subtree loading: subtree "ps/ss/ss" only data.val2 should
	 * receive a value
	 */
	rc = settings_load_subtree("ps/ss/ss");
	zassert_true(rc == 0, "settings_load failed");
	err = (data.val1 == 0) && (data.val2 == 2) && (data.val3 == 0);
	zassert_true(err, "wrong data value found");
	/* commit is called only for val2_settings */
	err = (!data.en1) && (data.en2) && (!data.en3);
	zassert_true(err, "wrong data enable found");

	memset(&data, 0, sizeof(struct stored_data));
	/* test load_one: path "ps/ss/ss/val2". Only data.val2 should
	 * receive a value
	 */
	val = 2;
	settings_save_one("ps/ss/ss/val2", &val, sizeof(uint8_t));
	rc = settings_load_one("ps/ss/ss/val2", &data.val2, sizeof(uint8_t));
	zassert_true(rc >= 0, "settings_load_one failed");
	err = (data.val1 == 0) && (data.val2 == 2) && (data.val3 == 0);
	zassert_true(err, "wrong data value found %u != 2", data.val2);

	/* clean up by deregistering settings_handler */
	rc = settings_deregister(&val1_settings);
	zassert_true(rc, "deregistering val1_settings failed");

	rc = settings_deregister(&val2_settings);
	zassert_true(rc, "deregistering val2_settings failed");

	rc = settings_deregister(&val3_settings);
	zassert_true(rc, "deregistering val3_settings failed");
}

int val123_set(const char *key, size_t len,
	       settings_read_cb read_cb, void *cb_arg)
{
	int rc;
	uint8_t val;
	LOG_DBG("[%s] key=%s, len=%d", __func__, key, len);
	zassert_equal(1, len, "Unexpected size");

/*
 * read_cb is  .text.settings_nvs_read_fn
 * from zephyr/libzephyr.a(settings_nvs.c.obj)
 */
	rc = read_cb(cb_arg, &val, sizeof(val));
	zassert_equal(sizeof(val), rc, "read_cb failed");

	if (!strcmp("1", key)) {
		data.val1 = val;
		data.en1 = true;
		return 0;
	}
	if (!strcmp("2", key)) {
		data.val2 = val;
		data.en2 = true;
		return 0;
	}
	if (!strcmp("3", key)) {
		data.val3 = val;
		data.en3 = true;
		return 0;
	}

	zassert_unreachable("Unexpected key value: %s", key);

	return 0;
}

static struct settings_handler val123_settings = {
	.name = "val",
	.h_set = val123_set,
};

unsigned int direct_load_cnt;
uint8_t val_directly_loaded;

int direct_loader(
	const char *key,
	size_t len,
	settings_read_cb read_cb,
	void *cb_arg,
	void *param)
{
	int rc;
	uint8_t val;

	zassert_equal(0x1234, (size_t)param);

	zassert_equal(1, len);
	zassert_is_null(key, "Unexpected key: %s", key);


	zassert_not_null(cb_arg);
	rc = read_cb(cb_arg, &val, sizeof(val));
	zassert_equal(sizeof(val), rc);

	val_directly_loaded = val;
	direct_load_cnt += 1;
	return 0;
}


ZTEST(settings_functional, test_direct_loading)
{
	int rc;
	uint8_t val;
	LOG_DBG("[%s] entry", __func__);
	settings_subsys_init();
	val = 11;
	settings_save_one("val/1", &val, sizeof(uint8_t));
	val = 23;
	settings_save_one("val/2", &val, sizeof(uint8_t));
	val = 35;
	settings_save_one("val/3", &val, sizeof(uint8_t));

	rc = settings_register(&val123_settings);
	zassert_true(rc == 0);
	memset(&data, 0, sizeof(data));

	LOG_DBG("[%s] settings_load()", __func__);
	rc = settings_load();
	zassert_true(rc == 0);

	zassert_equal(11, data.val1);
	zassert_equal(23, data.val2);
	zassert_equal(35, data.val3);

	/* Load subtree */
	memset(&data, 0, sizeof(data));

	LOG_DBG("[%s] settings_load_subtree(val/2)", __func__);
	rc = settings_load_subtree("val/2");
	zassert_true(rc == 0);

	zassert_equal(0,  data.val1);
	zassert_equal(23, data.val2);
	zassert_equal(0,  data.val3);

	/* Direct loading now */
	memset(&data, 0, sizeof(data));
	val_directly_loaded = 0;
	direct_load_cnt = 0;
	LOG_DBG("[%s] settings_load_subtree_direct(val/2)", __func__);
	rc = settings_load_subtree_direct(
		"val/2",
		direct_loader,
		(void *)0x1234);
	zassert_true(rc == 0);
	zassert_equal(0, data.val1);
	zassert_equal(0, data.val2);
	zassert_equal(0, data.val3);

	zassert_equal(1, direct_load_cnt);
	zassert_equal(23, val_directly_loaded);
	settings_deregister(&val123_settings);
}

struct test_loading_data {
	const char *n;
	const char *v;
};

/* Final data */
static const struct test_loading_data data_final[] = {
	{ .n = "val/1", .v = "final 1" },
	{ .n = "val/2", .v = "final 2" },
	{ .n = "val/3", .v = "final 3" },
	{ .n = "val/4", .v = "final 4" },
	{ .n = NULL }
};

/* The counter of the callback called */
static unsigned int data_final_called[ARRAY_SIZE(data_final)];


static int filtered_loader(
	const char *key,
	size_t len,
	settings_read_cb read_cb,
	void *cb_arg)
{
	int rc;
	const char *next;
	char buf[32];
	const struct test_loading_data *ldata;

	LOG_INF("[%s]-- Called: %s\n", __func__, key);

	/* Searching for a element in an array */
	for (ldata = data_final; ldata->n; ldata += 1) {
		if (settings_name_steq(key, ldata->n, &next)) {
			break;
		}
	}
	zassert_not_null(ldata->n, "Unexpected data name: %s", key);
	zassert_is_null(next);
	zassert_equal(strlen(ldata->v) + 1, len, "e: \"%s\", a:\"%s\"", ldata->v, buf);
	zassert_true(len <= sizeof(buf));

	rc = read_cb(cb_arg, buf, len);
	zassert_equal(len, rc);

	zassert_false(strcmp(ldata->v, buf), "e: \"%s\", a:\"%s\"", ldata->v, buf);

	/* Count an element that was properly loaded */
	data_final_called[ldata - data_final] += 1;

	return 0;
}

static struct settings_handler filtered_loader_settings = {
	.name = "filtered_test",
	.h_set = filtered_loader,
};


static int direct_filtered_loader(
	const char *key,
	size_t len,
	settings_read_cb read_cb,
	void *cb_arg,
	void *param)
{
	zassert_equal(0x3456, (size_t)param);
	return filtered_loader(key, len, read_cb, cb_arg);
}


ZTEST(settings_functional, test_direct_loading_filter)
{
	int rc;
	const struct test_loading_data *ldata;
	const char *prefix = filtered_loader_settings.name;
	char buffer[48];
	size_t n;

	/* Duplicated data */
	static const struct test_loading_data data_duplicates[] = {
		{ .n = "val/1", .v = "dup abc" },
		{ .n = "val/2", .v = "dup 123" },
		{ .n = "val/3", .v = "dup 11" },
		{ .n = "val/4", .v = "dup 34" },
		{ .n = "val/1", .v = "dup 56" },
		{ .n = "val/2", .v = "dup 7890" },
		{ .n = "val/4", .v = "dup niety" },
		{ .n = "val/3", .v = "dup er" },
		{ .n = "val/3", .v = "dup super" },
		{ .n = "val/3", .v = "dup xxx" },
		{ .n = NULL }
	};

	settings_subsys_init();
	/* Data that is going to be deleted */
	strcpy(buffer, prefix);
	strcat(buffer, "/to_delete");
	settings_save_one(buffer, "1", 2);
	LOG_DBG("settings_delete(%s)", buffer);
	(void) settings_delete(buffer);

	LOG_DBG("saving all the data");
	/* Saving all the data */
	for (ldata = data_duplicates; ldata->n; ++ldata) {
		strcpy(buffer, prefix);
		strcat(buffer, "/");
		strcat(buffer, ldata->n);
		settings_save_one(buffer, ldata->v, strlen(ldata->v) + 1);
	}
	/* 上面for循环写的数据，会被下面的for循环覆盖掉 */
	for (ldata = data_final; ldata->n; ++ldata) {
		strcpy(buffer, prefix);
		strcat(buffer, "/");
		strcat(buffer, ldata->n);
		settings_save_one(buffer, ldata->v, strlen(ldata->v) + 1);
	}


	memset(data_final_called, 0, sizeof(data_final_called));

	LOG_DBG("settings_load_subtree_direct(%s)", prefix);
	rc = settings_load_subtree_direct(
		prefix,
		direct_filtered_loader,
		(void *)0x3456);
	zassert_equal(0, rc);

	/* Check if all the data was called */
	for (n = 0; data_final[n].n; ++n) {
		zassert_equal(1, data_final_called[n],
			"Unexpected number of calls (%u) of (%s) element",
			n, data_final[n].n);
	}

	/* 此处才注册，可见上面的 save 都会不调用回到函数 */
	rc = settings_register(&filtered_loader_settings);
	zassert_true(rc == 0);

	LOG_DBG("settings_load_subtree(%s)", prefix);
	rc = settings_load_subtree(prefix);
	zassert_equal(0, rc);

	/* Check if all the data was called */
	for (n = 0; data_final[n].n; ++n) {
		zassert_equal(2, data_final_called[n],
			"Unexpected number of calls (%u) of (%s) element",
			n, data_final[n].n);
	}
	settings_deregister(&filtered_loader_settings);
}
