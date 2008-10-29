#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h> /* ntohl */
#include <string.h>

#include "reglib.h"

/* Intersects regulatory domains, this will skip any regulatory marked with
 * an alpha2 of '00', which is used to indicate a regulatory domain */

#define BUG_ON(foo) do { \
	if (foo) { \
		printf("BUG\n"); \
		exit(-1); \
	} \
	} while (0)

/* Helper for regdom_intersect(), this does the real
 * mathematical intersection fun */
static int reg_rules_intersect(
	struct ieee80211_reg_rule *rule1,
	struct ieee80211_reg_rule *rule2,
	struct ieee80211_reg_rule *intersected_rule)
{
	struct ieee80211_freq_range *freq_range1, *freq_range2, *freq_range;
	struct ieee80211_power_rule *power_rule1, *power_rule2, *power_rule;
	__u32 freq_diff;

	freq_range1 = &rule1->freq_range;
	freq_range2 = &rule2->freq_range;
	freq_range = &intersected_rule->freq_range;

	power_rule1 = &rule1->power_rule;
	power_rule2 = &rule2->power_rule;
	power_rule = &intersected_rule->power_rule;

	freq_range->start_freq_khz = max(freq_range1->start_freq_khz,
		freq_range2->start_freq_khz);
	freq_range->end_freq_khz = min(freq_range1->end_freq_khz,
		freq_range2->end_freq_khz);
	freq_range->max_bandwidth_khz = min(freq_range1->max_bandwidth_khz,
		freq_range2->max_bandwidth_khz);

	freq_diff = freq_range->end_freq_khz - freq_range->start_freq_khz;
	if (freq_range->max_bandwidth_khz > freq_diff)
		freq_range->max_bandwidth_khz = freq_diff;

	power_rule->max_eirp = min(power_rule1->max_eirp,
		power_rule2->max_eirp);
	power_rule->max_antenna_gain = min(power_rule1->max_antenna_gain,
		power_rule2->max_antenna_gain);

	intersected_rule->flags = (rule1->flags | rule2->flags);

	if (!is_valid_reg_rule(intersected_rule))
		return -EINVAL;

	return 0;
}

/**
 * regdom_intersect - do the intersection between two regulatory domains
 * @rd1: first regulatory domain
 * @rd2: second regulatory domain
 *
 * Use this function to get the intersection between two regulatory domains.
 * Once completed we will mark the alpha2 for the rd as intersected, "98",
 * as no one single alpha2 can represent this regulatory domain.
 *
 * Returns a pointer to the regulatory domain structure which will hold the
 * resulting intersection of rules between rd1 and rd2. We will
 * malloc() this structure for you.
 */
struct ieee80211_regdomain *regdom_intersect(
	struct ieee80211_regdomain *rd1,
	struct ieee80211_regdomain *rd2)
{
	int r, size_of_regd;
	unsigned int x, y;
	unsigned int num_rules = 0, rule_idx = 0;
	struct ieee80211_reg_rule *rule1, *rule2, *intersected_rule;
	struct ieee80211_regdomain *rd;
	/* This is just a dummy holder to help us count */
	struct ieee80211_reg_rule irule;

	/* Uses the stack temporarily for counter arithmetic */
	intersected_rule = &irule;

	memset(intersected_rule, 0, sizeof(struct ieee80211_reg_rule));

	if (!rd1 || !rd2) {
		fprintf(stderr, "rd1 or or rd2 is null\n");
		return NULL;
	}

	/* First we get a count of the rules we'll need, then we actually
	 * build them. This is to so we can malloc() and free() a
	 * regdomain once. The reason we use reg_rules_intersect() here
	 * is it will return -EINVAL if the rule computed makes no sense.
	 * All rules that do check out OK are valid. */

	for (x = 0; x < rd1->n_reg_rules; x++) {
		rule1 = &rd1->reg_rules[x];
		for (y = 0; y < rd2->n_reg_rules; y++) {
			rule2 = &rd2->reg_rules[y];
			if (!reg_rules_intersect(rule1, rule2,
					intersected_rule))
				num_rules++;
			memset(intersected_rule, 0,
					sizeof(struct ieee80211_reg_rule));
		}
	}

	if (!num_rules) {
		fprintf(stderr, "error: num_rules == 0\n");
		return NULL;
	}

	size_of_regd = sizeof(struct ieee80211_regdomain) +
		(num_rules * sizeof(struct ieee80211_reg_rule));

	rd = malloc(size_of_regd);
	if (!rd) {
		fprintf(stderr, "no memory left\n");
		return NULL;
	}

	memset(rd, 0, size_of_regd);

	for (x = 0; x < rd1->n_reg_rules; x++) {
		rule1 = &rd1->reg_rules[x];
		for (y = 0; y < rd2->n_reg_rules; y++) {
			rule2 = &rd2->reg_rules[y];
			/* This time around instead of using the stack lets
			 * write to the target rule directly saving ourselves
			 * a memcpy() */
			intersected_rule = &rd->reg_rules[rule_idx];
			r = reg_rules_intersect(rule1, rule2,
				intersected_rule);
			if (r)
				continue;
			rule_idx++;
		}
	}

	if (rule_idx != num_rules) {
		fprintf(stderr, "Error while doing regdom interesection :(\n");
		free(rd);
		return NULL;
	}

	rd->n_reg_rules = num_rules;
	rd->alpha2[0] = '9';
	rd->alpha2[1] = '9';

	return rd;
}

int main(int argc, char **argv)
{
	int fd;
	struct stat stat;
	__u8 *db;
	struct regdb_file_header *header;
	struct regdb_file_reg_country *countries;
	int dblen, siglen, num_countries, i, r = 0;
	struct ieee80211_regdomain *prev_world = NULL, *rd = NULL, *world = NULL;
	int intersected = 0;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("failed to open db file");
		return 2;
	}

	if (fstat(fd, &stat)) {
		perror("failed to fstat db file");
		return 2;
	}

	dblen = stat.st_size;

	db = mmap(NULL, dblen, PROT_READ, MAP_PRIVATE, fd, 0);
	if (db == MAP_FAILED) {
		perror("failed to mmap db file");
		return 2;
	}

	header = crda_get_file_ptr(db, dblen, sizeof(*header), 0);

	if (ntohl(header->magic) != REGDB_MAGIC) {
		fprintf(stderr, "Invalid database magic\n");
		return 2;
	}

	if (ntohl(header->version) != REGDB_VERSION) {
		fprintf(stderr, "Invalid database version\n");
		return 2;
	}

	siglen = ntohl(header->signature_length);
	/* adjust dblen so later sanity checks don't run into the signature */
	dblen -= siglen;

	if (dblen <= (int)sizeof(*header)) {
		fprintf(stderr, "Invalid signature length %d\n", siglen);
		return 2;
	}

	/* verify signature */
	if (!crda_verify_db_signature(db, dblen, siglen))
		return -EINVAL;

	num_countries = ntohl(header->reg_country_num);

	if (num_countries <= 0)
		return 0;

	countries = crda_get_file_ptr(db, dblen,
			sizeof(struct regdb_file_reg_country) * num_countries,
			header->reg_country_ptr);

	/* We intersect only when we have to rd structures ready */
	for (i = 0; i < num_countries; i++) {
		struct regdb_file_reg_country *country = countries + i;

		if (is_world_regdom((const char *) country->alpha2))
			continue;

		/* this is the previous country's rd, we don't care about it, we
		 * only care about the resulting intersected world rd, which
		 * should now be stored in prev_world, so we update that here */
		if (rd) {
			printf("Address of rd 2: %p -- about to free\n", (void *) rd);
			free(rd);
			printf("Address of rd 3: %p -- freed!\n", (void *) rd);
			rd = NULL;

			BUG_ON(!world);

			prev_world = world;
			world = NULL;
		} /* else prev_world is set correctly, first run,
		   * it was set to the first valid rd which is not
		   * the world regdomain, no need to update it */

		/* Gets the rd for the current country */
		rd = country2rd(db, dblen, country);
		if (!rd) {
			r = -ENOMEM;
			fprintf(stderr, "Could not covert country "
				"(%.2s) to rd\n", country->alpha2);
			goto out;
		}

		printf("Address of rd 1: %p -- new allocation\n", (void *) rd);

		/* On our first iteration on the first valid rd for a country
		 * we take that rd as the first world regdomain.
		 * We only hit this on our first iteration of a valid rd */
		if (!prev_world) {
			prev_world = rd;
			rd = NULL;
			continue;
		}

		/* If this is our first time around prev_world is
		 * the first regdomain, and rd is the second. If
		 * this is our third time around prev_world is the
		 * previous intersected world regdom of the first
		 * two rds */
		world = regdom_intersect(prev_world, rd);
		if (!world) {
			/* Could be something else but we'll live with this */
			r = -ENOMEM;
			if (intersected)
				fprintf(stderr, "Could not intersect world "
					"with country (%.2s)\n",
					rd->alpha2);
			else
				fprintf(stderr, "Could not intersect country (%.2s) "
					"with country (%.2s)\n",
					prev_world->alpha2,
					rd->alpha2);
			goto out;
		}

		if (intersected)
			/* Use UTF-8 Intersection symbol ? (0xE2,0x88,0xA9) :) */
			printf("WW (%d) intersect %c%c (%d) ==> %d rules\n",
				prev_world->n_reg_rules,
				rd->alpha2[0],
				rd->alpha2[1],
				rd->n_reg_rules,
				world->n_reg_rules);
		else
			printf("%c%c (%d) intersect %c%c (%d) ==> %d rules\n",
				prev_world->alpha2[0],
				prev_world->alpha2[1],
				prev_world->n_reg_rules,
				rd->alpha2[0],
				rd->alpha2[1],
				rd->n_reg_rules,
				world->n_reg_rules);
		intersected++;
	}

	if (intersected > 1)
		printf("%d regulatory domains intersected\n", intersected);
	else
		printf("Only one regulatory domain found\n");

	/* Tada! */
	printf("== World regulatory domain: ==\n");
	print_regdom(world);

out:
	if (world)
		free(world);
	if (rd)
		free(rd);
	if (prev_world)
		free(prev_world);

	return r;
}
