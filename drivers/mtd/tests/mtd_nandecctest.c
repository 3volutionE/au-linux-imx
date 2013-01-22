#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/mtd/nand_ecc.h>

/*
 * Test the implementation for software ECC
 *
 * No actual MTD device is needed, So we don't need to warry about losing
 * important data by human error.
 *
 * This covers possible patterns of corruption which can be reliably corrected
 * or detected.
 */

#if defined(CONFIG_MTD_NAND) || defined(CONFIG_MTD_NAND_MODULE)

struct nand_ecc_test {
	const char *name;
	void (*prepare)(void *, void *, void *, void *, const size_t);
	int (*verify)(void *, void *, void *, const size_t);
};

/*
 * The reason for this __change_bit_le() instead of __change_bit() is to inject
 * bit error properly within the region which is not a multiple of
 * sizeof(unsigned long) on big-endian systems
 */
#ifdef __LITTLE_ENDIAN
#define __change_bit_le(nr, addr) __change_bit(nr, addr)
#elif defined(__BIG_ENDIAN)
#define __change_bit_le(nr, addr) \
		__change_bit((nr) ^ ((BITS_PER_LONG - 1) & ~0x7), addr)
#else
#error "Unknown byte order"
#endif

static void single_bit_error_data(void *error_data, void *correct_data,
				size_t size)
{
	unsigned int offset = random32() % (size * BITS_PER_BYTE);

	memcpy(error_data, correct_data, size);
	__change_bit_le(offset, error_data);
}

static void no_bit_error(void *error_data, void *error_ecc,
		void *correct_data, void *correct_ecc, const size_t size)
{
	memcpy(error_data, correct_data, size);
	memcpy(error_ecc, correct_ecc, 3);
}

static int no_bit_error_verify(void *error_data, void *error_ecc,
				void *correct_data, const size_t size)
{
	unsigned char calc_ecc[3];
	int ret;

	__nand_calculate_ecc(error_data, size, calc_ecc);
	ret = __nand_correct_data(error_data, error_ecc, calc_ecc, size);
	if (ret == 0 && !memcmp(correct_data, error_data, size))
		return 0;

	return -EINVAL;
}

static void single_bit_error_in_data(void *error_data, void *error_ecc,
		void *correct_data, void *correct_ecc, const size_t size)
{
	single_bit_error_data(error_data, correct_data, size);
	memcpy(error_ecc, correct_ecc, 3);
}

static int single_bit_error_correct(void *error_data, void *error_ecc,
				void *correct_data, const size_t size)
{
	unsigned char calc_ecc[3];
	int ret;

	__nand_calculate_ecc(error_data, size, calc_ecc);
	ret = __nand_correct_data(error_data, error_ecc, calc_ecc, size);
	if (ret == 1 && !memcmp(correct_data, error_data, size))
		return 0;

	return -EINVAL;
}

static const struct nand_ecc_test nand_ecc_test[] = {
	{
		.name = "no-bit-error",
		.prepare = no_bit_error,
		.verify = no_bit_error_verify,
	},
	{
		.name = "single-bit-error-in-data-correct",
		.prepare = single_bit_error_in_data,
		.verify = single_bit_error_correct,
	},
};

static void dump_data_ecc(void *error_data, void *error_ecc, void *correct_data,
			void *correct_ecc, const size_t size)
{
	pr_info("hexdump of error data:\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 4,
			error_data, size, false);
	print_hex_dump(KERN_INFO, "hexdump of error ecc: ",
			DUMP_PREFIX_NONE, 16, 1, error_ecc, 3, false);

	pr_info("hexdump of correct data:\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 4,
			correct_data, size, false);
	print_hex_dump(KERN_INFO, "hexdump of correct ecc: ",
			DUMP_PREFIX_NONE, 16, 1, correct_ecc, 3, false);
}

static int nand_ecc_test_run(const size_t size)
{
	int i;
	int err = 0;
	void *error_data;
	void *error_ecc;
	void *correct_data;
	void *correct_ecc;

	error_data = kmalloc(size, GFP_KERNEL);
	error_ecc = kmalloc(3, GFP_KERNEL);
	correct_data = kmalloc(size, GFP_KERNEL);
	correct_ecc = kmalloc(3, GFP_KERNEL);

	if (!error_data || !error_ecc || !correct_data || !correct_ecc) {
		err = -ENOMEM;
		goto error;
	}

	get_random_bytes(correct_data, size);
	__nand_calculate_ecc(correct_data, size, correct_ecc);

	for (i = 0; i < ARRAY_SIZE(nand_ecc_test); i++) {
		nand_ecc_test[i].prepare(error_data, error_ecc,
				correct_data, correct_ecc, size);
		err = nand_ecc_test[i].verify(error_data, error_ecc,
						correct_data, size);

		if (err) {
			pr_err("mtd_nandecctest: not ok - %s-%zd\n",
				nand_ecc_test[i].name, size);
			dump_data_ecc(error_data, error_ecc,
				correct_data, correct_ecc, size);
			break;
		}
		pr_info("mtd_nandecctest: ok - %s-%zd\n",
			nand_ecc_test[i].name, size);
	}
error:
	kfree(error_data);
	kfree(error_ecc);
	kfree(correct_data);
	kfree(correct_ecc);

	return err;
}

#else

static int nand_ecc_test_run(const size_t size)
{
	return 0;
}

#endif

static int __init ecc_test_init(void)
{
	int err;

	err = nand_ecc_test_run(256);
	if (err)
		return err;

	return nand_ecc_test_run(512);
}

static void __exit ecc_test_exit(void)
{
}

module_init(ecc_test_init);
module_exit(ecc_test_exit);

MODULE_DESCRIPTION("NAND ECC function test module");
MODULE_AUTHOR("Akinobu Mita");
MODULE_LICENSE("GPL");
