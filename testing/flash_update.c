// SPDX-License-Identifier: GPL-2.0-or-later

/* Only the production translation unit uses the fault-injecting allocators. */
#undef malloc
#undef calloc
#undef realloc

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <flash/nor/imp.h>
#include <target/image.h>

#define FLASH_BASE 0x1000
#define BANK_SIZE 0x100
#define SECTOR_SIZE 0x10
#define SECTOR_COUNT (BANK_SIZE / SECTOR_SIZE)
#define BANK_COUNT 4
#define SECTION_COUNT 8

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

struct image_section_spec {
	target_addr_t address;
	uint32_t size;
};

static struct target test_target;
static struct image test_image;
static struct imagesection image_sections[SECTION_COUNT];
static uint8_t image_data[SECTION_COUNT][BANK_COUNT * BANK_SIZE];
static uint8_t flash_data[BANK_COUNT * BANK_SIZE];
static unsigned int erased[BANK_COUNT][SECTOR_COUNT];
static unsigned int erase_calls;
static unsigned int unlock_calls;
static unsigned int write_calls;
static unsigned int dirty_calls;
static unsigned int checksum_calls;
static unsigned int warning_count;
static unsigned int error_count;
static unsigned int allocation_count;
static unsigned int fail_allocation;
static unsigned int fail_checksum;
static bool short_read;
static bool read_error;
static bool erase_error;
static bool unlock_error;
static bool write_error;
static bool corrupt_write;
static unsigned int unlocked[BANK_COUNT][SECTOR_COUNT];
static char operations[64];
static size_t operation_count;

int debug_level = LOG_LVL_SILENT;

void *flash_test_malloc(size_t size)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return malloc(size);
}

void *flash_test_calloc(size_t count, size_t size)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return calloc(count, size);
}

void *flash_test_realloc(void *ptr, size_t size)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return realloc(ptr, size);
}

void log_printf_lf(enum log_levels level, const char *file, unsigned int line,
		const char *function, const char *format, ...)
{
	if (level == LOG_LVL_ERROR)
		error_count++;
	else if (level == LOG_LVL_WARNING)
		warning_count++;
}

unsigned int get_flash_name_index(const char *name)
{
	return 0;
}

bool flash_driver_name_matches(const char *name, const char *expected)
{
	return strcmp(name, expected) == 0;
}

void flash_set_dirty(void)
{
	dirty_calls++;
	for (struct flash_bank *bank = flash_bank_list(); bank; bank = bank->next) {
		for (unsigned int i = 0; i < bank->num_sectors; i++)
			bank->sectors[i].is_erased = -1;
	}
}

static uint8_t *flash_buffer(target_addr_t address, uint32_t size)
{
	CHECK(address >= FLASH_BASE);
	CHECK(address - FLASH_BASE <= sizeof(flash_data));
	CHECK(size <= sizeof(flash_data) - (address - FLASH_BASE));
	return flash_data + (address - FLASH_BASE);
}

int target_read_buffer(struct target *target, target_addr_t address,
		uint32_t size, uint8_t *buffer)
{
	memcpy(buffer, flash_buffer(address, size), size);
	return ERROR_OK;
}

int target_read_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	return target_read_buffer(target, address, size * count, buffer);
}

int target_blank_check_memory(struct target *target,
		struct target_memory_check_block *blocks, unsigned int num_blocks,
		uint8_t erased_value, unsigned int *checked)
{
	return ERROR_NOT_IMPLEMENTED;
}

static uint32_t checksum(const uint8_t *buffer, uint32_t size)
{
	uint32_t crc = 2166136261U;
	for (uint32_t i = 0; i < size; i++)
		crc = (crc ^ buffer[i]) * 16777619U;
	return crc;
}

int image_calculate_checksum(const uint8_t *buffer, uint32_t size, uint32_t *crc)
{
	*crc = checksum(buffer, size);
	return ERROR_OK;
}

int target_checksum_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t *crc)
{
	*crc = checksum(flash_buffer(address, size), size);
	return ERROR_OK;
}

int target_checksum_memory_blocks(struct target *target,
		struct target_memory_checksum_block *blocks, unsigned int num_blocks)
{
	CHECK(target == &test_target);
	CHECK(num_blocks > 0);
	if (++checksum_calls == fail_checksum)
		return ERROR_FAIL;
	for (unsigned int i = 0; i < num_blocks; i++)
		blocks[i].crc = checksum(flash_buffer(blocks[i].address, blocks[i].size), blocks[i].size);
	return ERROR_OK;
}

int image_read_section(struct image *image, int section, target_addr_t offset,
		uint32_t size, uint8_t *buffer, size_t *size_read)
{
	CHECK(section >= 0);
	CHECK((unsigned int)section < image->num_sections);
	CHECK(offset + size <= image->sections[section].size);
	if (read_error)
		return ERROR_IMAGE_FORMAT_ERROR;
	*size_read = short_read && size ? size - 1 : size;
	memcpy(buffer, (uint8_t *)image->sections[section].private + offset, *size_read);
	return ERROR_OK;
}

static int mock_probe(struct flash_bank *bank)
{
	return ERROR_OK;
}

static void record_operation(char operation)
{
	CHECK(operation_count + 1 < sizeof(operations));
	operations[operation_count++] = operation;
}

static int mock_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
	CHECK(!set);
	CHECK(first <= last);
	CHECK(last < (bank->num_prot_blocks ? bank->num_prot_blocks : bank->num_sectors));
	record_operation('u');
	unlock_calls++;
	if (unlock_error)
		return ERROR_FLASH_PROTECTED;
	for (unsigned int i = first; i <= last; i++)
		unlocked[bank->bank_number][i]++;
	return ERROR_OK;
}

static int mock_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	CHECK(first <= last);
	CHECK(last < bank->num_sectors);
	record_operation('e');
	erase_calls++;
	if (erase_error)
		return ERROR_FLASH_PROTECTED;
	for (unsigned int i = first; i <= last; i++) {
		erased[bank->bank_number][i]++;
		memset(flash_buffer(bank->base + bank->sectors[i].offset, bank->sectors[i].size),
			bank->erased_value, bank->sectors[i].size);
	}
	return ERROR_OK;
}

static int mock_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t size)
{
	CHECK(offset + size <= bank->size);
	CHECK(size > 0);
	record_operation('w');
	write_calls++;
	if (write_error)
		return ERROR_FLASH_OPERATION_FAILED;
	uint8_t *destination = flash_buffer(bank->base + offset, size);
	memcpy(destination, buffer, size);
	if (corrupt_write)
		destination[size - 1] ^= 1;
	return ERROR_OK;
}

static const struct flash_driver mock_driver = {
	.name = "flash-update-test",
	.protect = mock_protect,
	.erase = mock_erase,
	.write = mock_write,
	.auto_probe = mock_probe,
	.free_driver_priv = default_flash_free_driver_priv,
};

static void setup(unsigned int bank_count)
{
	fail_allocation = 0;
	flash_free_all_banks();
	memset(&test_target, 0, sizeof(test_target));
	test_target.state = TARGET_HALTED;
	memset(flash_data, 0xff, sizeof(flash_data));
	memset(erased, 0, sizeof(erased));
	memset(unlocked, 0, sizeof(unlocked));
	memset(operations, 0, sizeof(operations));
	operation_count = 0;
	erase_calls = 0;
	unlock_calls = 0;
	write_calls = 0;
	dirty_calls = 0;
	checksum_calls = 0;
	warning_count = 0;
	error_count = 0;
	fail_checksum = 0;
	short_read = false;
	read_error = false;
	erase_error = false;
	unlock_error = false;
	write_error = false;
	corrupt_write = false;
	for (unsigned int i = 0; i < bank_count; i++) {
		struct flash_bank *bank = calloc(1, sizeof(*bank));
		CHECK(bank);
		bank->base = FLASH_BASE + BANK_SIZE * i;
		bank->size = BANK_SIZE;
		bank->num_sectors = SECTOR_COUNT;
		bank->sectors = alloc_block_array(0, SECTOR_SIZE, SECTOR_COUNT);
		CHECK(bank->sectors);
		bank->target = &test_target;
		bank->driver = &mock_driver;
		bank->erased_value = 0xff;
		bank->default_padded_value = 0xff;
		bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
		flash_bank_add(bank);
	}
	allocation_count = 0;
}

static void set_image(const struct image_section_spec *spec, unsigned int count)
{
	CHECK(count <= SECTION_COUNT);
	test_image = (struct image) {
		.num_sections = count,
		.sections = count ? image_sections : NULL,
	};
	for (unsigned int i = 0; i < count; i++) {
		CHECK(spec[i].size <= sizeof(image_data[i]));
		memset(image_data[i], 0x11 * (i + 1), sizeof(image_data[i]));
		image_sections[i] = (struct imagesection) {
			.base_address = spec[i].address,
			.size = spec[i].size,
			.private = image_data[i],
		};
	}
}

static int update(void)
{
	return flash_write_changed_unlock_verify(&test_target, &test_image, true, false, true);
}

static void check_image(void)
{
	for (unsigned int i = 0; i < test_image.num_sections; i++) {
		struct imagesection *section = &test_image.sections[i];
		if (section->size)
			CHECK(!memcmp(flash_buffer(section->base_address, section->size),
				section->private, section->size));
	}
	for (unsigned int i = 0; i < BANK_COUNT; i++) {
		for (unsigned int j = 0; j < SECTOR_COUNT; j++)
			CHECK(erased[i][j] <= 1);
	}
}

static void copy_image(void)
{
	for (unsigned int i = 0; i < test_image.num_sections; i++) {
		struct imagesection *section = &test_image.sections[i];
		memcpy(flash_buffer(section->base_address, section->size),
			section->private, section->size);
	}
}

static void test_sections(void)
{
	static const struct {
		const char *name;
		unsigned int banks;
		unsigned int sections;
		struct image_section_spec spec[4];
	} cases[] = {
		{ "single section", 1, 1, {{0x1000, 16}} },
		{ "adjacent sections", 1, 2, {{0x1000, 16}, {0x1010, 16}} },
		{ "bank endpoint", 1, 1, {{0x10f0, 16}} },
		{ "last section spanning banks", 2, 1, {{0x10f0, 32}} },
		{ "single section spanning four banks", 4, 1, {{0x10f0, 0x220}} },
		{ "nonfinal section spanning banks", 3, 2, {{0x10f0, 32}, {0x1200, 16}} },
		{ "separate banks", 3, 3, {{0x1000, 16}, {0x1100, 16}, {0x1200, 16}} },
		{ "gap before final section", 1, 2, {{0x1000, 16}, {0x1040, 16}} },
		{ "gap before nonfinal section", 1, 3, {{0x1000, 16}, {0x1040, 16}, {0x1050, 16}} },
		{ "unsorted sections", 1, 3, {{0x1080, 16}, {0x1000, 16}, {0x1040, 16}} },
		{ "empty sections", 1, 4, {{0x1010, 0}, {0x1000, 16}, {0, 0}, {0x1040, 16}} },
		{ "partial sectors", 1, 1, {{0x1003, 35}} },
		{ "empty image", 1, 0, {{0, 0}} },
		{ "only empty sections", 1, 2, {{0, 0}, {0x1000, 0}} },
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		setup(cases[i].banks);
		set_image(cases[i].spec, cases[i].sections);
		CHECK(update() == ERROR_OK);
		check_image();
		CHECK(checksum_calls == (write_calls ? 2U : 0U));
		for (unsigned int j = 0; j < cases[i].sections; j++) {
			CHECK(image_sections[j].base_address == cases[i].spec[j].address);
			CHECK(image_sections[j].size == cases[i].spec[j].size);
		}
		printf("PASS: %s\n", cases[i].name);
	}
}

static void test_padding(void)
{
	setup(1);
	struct flash_bank *bank = flash_bank_list();
	bank->write_start_alignment = 4;
	bank->write_end_alignment = 4;
	bank->default_padded_value = 0x5a;
	set_image((struct image_section_spec[]){{0x1003, 14}}, 1);
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(flash_data[0] == 0x5a && flash_data[2] == 0x5a);
	CHECK(flash_data[17] == 0x5a && flash_data[19] == 0x5a);

	setup(1);
	bank = flash_bank_list();
	bank->write_start_alignment = FLASH_WRITE_ALIGN_SECTOR;
	bank->write_end_alignment = FLASH_WRITE_ALIGN_SECTOR;
	bank->default_padded_value = 0;
	set_image((struct image_section_spec[]){{0x1003, 14}}, 1);
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(flash_data[0] == 0 && flash_data[31] == 0);

	setup(1);
	flash_bank_list()->minimal_write_gap = 1;
	set_image((struct image_section_spec[]){{0x1000, 2}, {0x1008, 2}, {0x1010, 2}}, 3);
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(erase_calls == 2);

	setup(1);
	flash_bank_list()->minimal_write_gap = FLASH_WRITE_CONTINUOUS;
	flash_bank_list()->default_padded_value = 0x5a;
	set_image((struct image_section_spec[]){{0x1000, 2}, {0x1080, 2}}, 2);
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(flash_data[0x40] == 0x5a);
	CHECK(erase_calls == 1);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 2}, {0x1080, 2}}, 2);
	flash_data[0x40] = 0x5a;
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(flash_data[0x40] == 0x5a);
	puts("PASS: alignment, padding and shared erase sectors");
}

static void test_changed_sectors(void)
{
	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 80}}, 1);
	copy_image();
	CHECK(update() == ERROR_OK);
	CHECK(!erase_calls && !write_calls && checksum_calls == 2);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 80}}, 1);
	copy_image();
	flash_data[0] ^= 1;
	flash_data[32] ^= 1;
	flash_data[64] ^= 1;
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(erase_calls == 3 && write_calls == 3 && checksum_calls == 2);
	CHECK(!erased[0][1] && !erased[0][3]);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 80}}, 1);
	copy_image();
	flash_data[16] ^= 1;
	flash_data[32] ^= 1;
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(erase_calls == 1 && write_calls == 1);
	CHECK(!erased[0][0] && !erased[0][3] && !erased[0][4]);
	puts("PASS: unchanged, alternating and consecutive changed sectors");
}

static void test_options(void)
{
	static const char * const expected_operations[] = {"w", "ew", "uw", "uew"};
	for (unsigned int flags = 0; flags < 8; flags++) {
		bool erase = flags & 1;
		bool unlock = flags & 2;
		bool verify = flags & 4;
		setup(1);
		set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
		CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
			erase, unlock, verify) == ERROR_OK);
		check_image();
		CHECK(erase_calls == (erase ? 1U : 0U));
		CHECK(unlock_calls == (unlock ? 1U : 0U));
		CHECK(dirty_calls == (erase ? 1U : 0U));
		CHECK(write_calls == 1 && checksum_calls == (verify ? 2U : 1U));
		CHECK(!strcmp(operations, expected_operations[flags & 3]));
	}

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	copy_image();
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		true, true, true) == ERROR_OK);
	CHECK(!erase_calls && !unlock_calls && !write_calls && checksum_calls == 2);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	copy_image();
	fail_checksum = 1;
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		false, false, false) == ERROR_OK);
	check_image();
	CHECK(!erase_calls && !unlock_calls && write_calls == 1);
	CHECK(checksum_calls == 1 && warning_count == 1);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	corrupt_write = true;
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		false, false, false) == ERROR_OK);
	CHECK(checksum_calls == 1 && !error_count);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	unlock_error = true;
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		true, true, true) == ERROR_FLASH_PROTECTED);
	CHECK(unlock_calls == 1 && !erase_calls && !write_calls && error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1033, 2}}, 1);
	struct flash_bank *bank = flash_bank_list();
	bank->num_prot_blocks = SECTOR_COUNT / 2;
	bank->prot_blocks = alloc_block_array(0, SECTOR_SIZE * 2, bank->num_prot_blocks);
	CHECK(bank->prot_blocks);
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		true, true, true) == ERROR_OK);
	check_image();
	CHECK(unlock_calls == 1 && unlocked[0][1] == 1 && !unlocked[0][3]);
	CHECK(!strcmp(operations, "uew"));

	setup(4);
	set_image((struct image_section_spec[]){{0x10f0, 0x220}}, 1);
	CHECK(flash_write_changed_unlock_verify(&test_target, &test_image,
		false, true, true) == ERROR_OK);
	check_image();
	CHECK(unlock_calls == 4 && write_calls == 4 && !erase_calls && checksum_calls == 2);
	puts("PASS: erase, unlock and verify options, ordering and protection blocks");
}

static void test_errors(void)
{
	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	copy_image();
	fail_checksum = 1;
	CHECK(update() == ERROR_OK);
	check_image();
	CHECK(erase_calls == 1 && warning_count == 1 && checksum_calls == 2);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	fail_checksum = 2;
	CHECK(update() != ERROR_OK);
	CHECK(error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	corrupt_write = true;
	CHECK(update() != ERROR_OK);
	CHECK(error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	erase_error = true;
	CHECK(update() == ERROR_FLASH_PROTECTED);
	CHECK(!write_calls);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	write_error = true;
	CHECK(update() == ERROR_FLASH_OPERATION_FAILED);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	read_error = true;
	CHECK(update() == ERROR_IMAGE_FORMAT_ERROR);
	CHECK(!erase_calls);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	short_read = true;
	CHECK(update() != ERROR_OK);
	CHECK(!erase_calls && error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}, {0x1008, 16}}, 2);
	CHECK(update() != ERROR_OK);
	CHECK(!erase_calls && error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}, {0x1100, 16}}, 2);
	CHECK(update() != ERROR_OK);
	CHECK(!erase_calls && error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{UINT64_MAX - 7, 16}}, 1);
	CHECK(update() != ERROR_OK);
	CHECK(!erase_calls && error_count > 0);

	setup(1);
	set_image((struct image_section_spec[]){{0x1000, 16}}, 1);
	flash_bank_list()->sectors[0].offset = 1;
	CHECK(update() == ERROR_FLASH_SECTOR_INVALID);
	CHECK(!erase_calls && error_count > 0);

	setup(1);
	CHECK(flash_write_changed_unlock_verify(NULL, &test_image, true, false, true) != ERROR_OK);
	CHECK(flash_write_changed_unlock_verify(&test_target, NULL, true, false, true) != ERROR_OK);
	test_image = (struct image){.num_sections = 1};
	CHECK(update() != ERROR_OK);
	CHECK(!erase_calls && error_count == 3);
	puts("PASS: checksum, driver, image and input errors");
}

static void test_allocation_failures(void)
{
	static const struct image_section_spec spec[] = {
		{0x10f0, 0x220}, {0x1000, 16}, {0x1040, 16},
	};
	setup(4);
	set_image(spec, ARRAY_SIZE(spec));
	CHECK(update() == ERROR_OK);
	check_image();
	unsigned int allocations = allocation_count;
	for (unsigned int i = 1; i <= allocations; i++) {
		setup(4);
		set_image(spec, ARRAY_SIZE(spec));
		fail_allocation = i;
		CHECK(update() != ERROR_OK);
		CHECK(!erase_calls && !write_calls && error_count > 0);
	}
	printf("PASS: all %u allocation failure points\n", allocations);
}

int main(void)
{
	test_sections();
	test_padding();
	test_changed_sectors();
	test_options();
	test_errors();
	test_allocation_failures();
	fail_allocation = 0;
	flash_free_all_banks();
	return EXIT_SUCCESS;
}
