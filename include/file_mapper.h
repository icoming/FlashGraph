#ifndef __FILE_BLOCK_MAPPER_H__
#define __FILE_BLOCK_MAPPER_H__

#include <vector>
#include <string>

#include "common.h"
#include "parameters.h"
#include "concurrency.h"
#include "exception.h"

const int FILE_CONST_A = 31;
const int FILE_CONST_P = 191;

struct block_identifier
{
	int idx;		// identify the file where the block is.
	off_t off;		// the location (in pages) in the file.
};

class file_mapper
{
	static atomic_integer file_id_gen;
	int file_id;
	std::vector<file_info> files;
protected:
	const std::vector<file_info> &get_files() const {
		return files;
	}
public:
	const int STRIPE_BLOCK_SIZE;
public:
	file_mapper(const std::vector<file_info> &files,
			int block_size): STRIPE_BLOCK_SIZE(block_size) {
		ASSERT_TRUE(block_size > 0);
		this->files = files;
		file_id = file_id_gen.inc(1);
	}

	virtual ~file_mapper() {
	}

	int get_file_id() const {
		return file_id;
	}

	const std::string &get_file_name(int idx) const {
		return files[idx].name;
	}

	int get_file_node_id(int idx) const {
		return files[idx].node_id;
	}

	int get_num_files() const {
		return (int) files.size();
	}

	virtual void map(off_t, struct block_identifier &) const = 0;
	virtual int map2file(off_t) const = 0;
	virtual off_t map_backwards(int idx, off_t off_in_file) const = 0;

	virtual file_mapper *clone() = 0;
};

int gen_RAID_rand_start(int num_files);

class RAID0_mapper: public file_mapper
{
	static int rand_start;
public:
	RAID0_mapper(const std::vector<file_info> &files,
			int block_size): file_mapper(files, block_size) {
#ifdef TEST
		if (rand_start == 0) {
			rand_start = gen_RAID_rand_start(files.size());
			printf("rand start shift in RAID0: %d\n", rand_start);
		}
#endif
	}

	virtual void map(off_t off, struct block_identifier &bid) const {
		int idx_in_block = off % STRIPE_BLOCK_SIZE;
		off_t block_idx = off / STRIPE_BLOCK_SIZE;
		bid.idx = (int) ((block_idx + rand_start) % get_num_files());
		bid.off = block_idx / get_num_files() * STRIPE_BLOCK_SIZE
			+ idx_in_block;
	}

	virtual int map2file(off_t off) const {
		return (int) (((off / STRIPE_BLOCK_SIZE) + rand_start) % get_num_files());
	}

	virtual off_t map_backwards(int idx, off_t off_in_file) const {
		int idx_in_block = off_in_file % STRIPE_BLOCK_SIZE;
		off_t block_in_file = off_in_file / STRIPE_BLOCK_SIZE;
		return (block_in_file * get_num_files() + (idx - rand_start
					+ get_num_files()) % get_num_files()) * STRIPE_BLOCK_SIZE
			+ idx_in_block;
	}

	virtual file_mapper *clone() {
		return new RAID0_mapper(get_files(), STRIPE_BLOCK_SIZE);
	}
};

class RAID5_mapper: public file_mapper
{
	static int rand_start;
public:
	RAID5_mapper(const std::vector<file_info> &files,
			int block_size): file_mapper(files, block_size) {
#ifdef TEST
		if (rand_start == 0) {
			rand_start = gen_RAID_rand_start(files.size());
			printf("rand start shift RAID5: %d\n", rand_start);
		}
#endif
	}

	virtual void map(off_t off, struct block_identifier &bid) const {
		int idx_in_block = off % STRIPE_BLOCK_SIZE;
		off_t block_idx = off / STRIPE_BLOCK_SIZE;
		bid.idx = (int) (block_idx % get_num_files());
		bid.off = block_idx / get_num_files() * STRIPE_BLOCK_SIZE
			+ idx_in_block;
		int shift = (int) ((block_idx / get_num_files()) % get_num_files());
		bid.idx = (bid.idx + shift + rand_start) % get_num_files();
	}

	virtual int map2file(off_t off) const {
		off_t block_idx = off / STRIPE_BLOCK_SIZE;
		int shift = (int) ((block_idx / get_num_files()) % get_num_files());
		return (int) ((block_idx % get_num_files() + shift
					+ rand_start) % get_num_files());
	}

	virtual off_t map_backwards(int idx, off_t off_in_file) const {
		int idx_in_block = off_in_file % STRIPE_BLOCK_SIZE;
		off_t block_in_file = off_in_file / STRIPE_BLOCK_SIZE;
		int idx_in_stripe = (rand_start + block_in_file) % get_num_files();
		return (block_in_file * get_num_files() + (idx - idx_in_stripe
					+ get_num_files()) % get_num_files()) * STRIPE_BLOCK_SIZE
			+ idx_in_block;
	}

	virtual file_mapper *clone() {
		return new RAID5_mapper(get_files(), STRIPE_BLOCK_SIZE);
	}
};

class hash_mapper: public file_mapper
{
	static const int CONST_A = FILE_CONST_A;
	static const int CONST_P = FILE_CONST_P;
	const int P_MOD_N;

	int cycle_size_in_bucket(int idx) const {
		return idx < P_MOD_N ? (CONST_P / get_num_files()
				+ 1) : (CONST_P / get_num_files());
	}
public:
	hash_mapper(const std::vector<file_info> &files, int block_size): file_mapper(
				files, block_size), P_MOD_N(CONST_P % files.size()) {
	}

	virtual void map(off_t off, struct block_identifier &bid) const {
		int idx_in_block = off % STRIPE_BLOCK_SIZE;
		off_t block_idx = off / STRIPE_BLOCK_SIZE;
		off_t p_idx = (CONST_A * block_idx) % CONST_P;
		bid.idx = p_idx % get_num_files();
		off_t cycle_idx = block_idx / CONST_P;
		int cycle_len_in_bucket = cycle_size_in_bucket(bid.idx);
		// length of all previous cycles
		bid.off = (cycle_idx * cycle_len_in_bucket
				// location in the current cycle
				+ p_idx / get_num_files())
			* STRIPE_BLOCK_SIZE + idx_in_block;
	}

	virtual int map2file(off_t off) const {
		off_t block_idx = off / STRIPE_BLOCK_SIZE;
		off_t p_idx = (CONST_A * block_idx) % CONST_P;
		return p_idx % get_num_files();
	}

	virtual off_t map_backwards(int idx, off_t off_in_file) const {
		throw unsupported_exception();
	}

	virtual file_mapper *clone() {
		return new hash_mapper(get_files(), STRIPE_BLOCK_SIZE);
	}
};

#endif
