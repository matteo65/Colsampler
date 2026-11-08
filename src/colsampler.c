/*
 * colsampler.c
 *
 * Copyright(C) 2026 Matteo Zapparoli - zapparoli.matteo@gmail.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compile: gcc -O3 colsampler.c -o colsampler -lm
 *
 * Syntax:
 *  colsampler [small | large | huge [<max collisions> [<max TB>]]]
 *
 * No args prints help
 *
 * Algorithm Description:
 *  This program estimates the quality of a 64-bit Pseudorandom Number Generator
 *  (PRNG) by analyzing a continuous native-endian uint64_t stream of binary
 *  numbers from stdin and detecting exact 64-bit collisions using a fast 
 *  direct-addressing table.
 *
 *  1. Filtering Phase:
 *     For each 64-bit integer read from the stream, the algorithm extracts:
 *     - The 32 least significant bits (LSB), used as an index.
 *     - The 32 most significant bits (MSB), used as the value.
 *
 *     Three execution modes are available:
 *
 *     - small : Only values whose LSB belongs to [0, 2^30 - 1] are processed.
 *               This uses a lookup table of 2^30 entries (4 GiB).
 *
 *     - large : Only values whose LSB belongs to [0, 2^31 - 1] are processed.
 *               This uses a lookup table of 2^31 entries (8 GiB).
 *
 *     - huge  : All LSB values are accepted.
 *               This uses a lookup table of 2^32 entries (16 GiB).
 *
 *     In all modes, values whose MSB is zero are ignored because zero is
 *     reserved as the sentinel value representing an unused table entry.
 *
 *  2. First-Come-First-Served (FCFS) Data Anchoring:
 *     The program maintains a pre-allocated direct-addressing table whose
 *     size depends on the selected execution mode.
 *
 *     - If the table slot corresponding to the extracted LSB is empty (0),
 *       the current MSB value is permanently anchored to that slot.
 *
 *     - If the slot already contains the same MSB value, an exact 64-bit
 *       collision is detected and recorded.
 *
 *     - If the slot contains a different MSB value, the value is ignored.
 *
 *     This First-Come-First-Served (FCFS) policy deterministically samples
 *     the stream according to arrival order while maintaining constant-time
 *     lookup and update operations.
 *
 *
 *  3. Statistical Validation (4-Sigma Poisson Check):
 *     Since exact collisions under uniform distribution are rare events,
 *     the expected collision count follows a Poisson distribution whose
 *     theoretical mean depends on the selected table size.
 *     Periodically and upon each collision, the program calculates the
 *     theoretical expected mean (Lambda) and its standard deviation (Sigma).
 *     If the observed collisions deviate from the expected mean by more than 4 Sigma
 *     (99.9937% confidence interval) in either direction (excessive collisions or
 *     artificial uniformity), the stream is flagged as an invalid PRNG and execution terminates.
 */

#include <stdio.h>
#include <stdint.h> // uint64_t
#include <stdlib.h> // exit(), strtoul(), strtod()
#include <stdarg.h> // vsnprintf()
#include <ctype.h> // isdigit()
#include <errno.h> // errno
#include <string.h> // strcmp()
#include <inttypes.h> // PRIu64
#include <time.h> // time()
#include <float.h> // DBL_MAX
#include <math.h> // expm1(), sqrt()

#if UINTPTR_MAX != UINT64_MAX
#error "Colsampler requires a 64-bit environment"
#endif

#define VERSION "1.0"

#define BUF_SIZE 16384
#define MSB_VALUES 4294967295.0 // 2^32 - 1
#define MAX_TB_LIMIT ((double)UINT64_MAX / 1000000000000.0)

enum model {SMALL, LARGE, HUGE};

// Array storing the 4 most significant bytes (MSB) of the processed numbers
static uint32_t *array = NULL;
static size_t array_length;
static double filter_probability;
static double M;
static enum model start_option;

// Prototypes
static void fatal_error(const char *frm,...);
static void failure(const char *frm,...);
static void copyright(void);
static void usage(void);
static char *time_interval(time_t t);
static void array_free(void);
static double collisions_expected(uint64_t filtered_count);
static double check_statistical_anomaly(uint64_t filtered_count, size_t real_colls);
static void check_filter_anomaly(uint64_t total_read, uint64_t filtered_count);
static int is_valid_size_t(const char *str, size_t *out_value);
static int is_valid_double(const char *str, double *out_value);
static uint64_t get_available_memory(void);

int main(int argc, char *argv[])
{
	uint64_t buffer[BUF_SIZE]; // Buffer to optimize reading from stdin
	uint64_t k = 0; // Total numbers read
	size_t n; // Counter for the number of items read from stdin in a single fread() call
	uint64_t r = 0; // Counter for number of fread() calls
	size_t colls = 0; // Total collisions
	uint64_t filtered_count = 0; // Total numbers passing the filter conditions

	// Default termination limits
	size_t max_collisions = 16;
	double max_TB = 2.0;

	if(argc <= 1) {
		usage();
		return EXIT_SUCCESS;
	}

	if(argc > 4) {
		fatal_error("Invalid arguments number");
	}

	if(strcmp(argv[1], "small") == 0) {
		start_option = SMALL;
	}
	else if(strcmp(argv[1], "large") == 0) {
		start_option = LARGE;
	}
	else if(strcmp(argv[1], "huge") == 0) {
		start_option = HUGE;
	}
	else {
		fatal_error("Unknown %s argument", argv[1]);
	}

	copyright();

	// Parse optional custom collision limit
	if(argc >= 3) {
		if(!is_valid_size_t(argv[2], &max_collisions)) {
			fatal_error("Invalid max_collisions value");
		}
	}

	// Parse optional custom TB data limit
	if(argc == 4) {
		if(!is_valid_double(argv[3], &max_TB) || max_TB > MAX_TB_LIMIT) {
			fatal_error("Invalid max_TB value");
		}
	}

	// Calculate execution byte limit (1 TB = 1000000000000 bytes)
	uint64_t max_bytes = (uint64_t)(max_TB * 1000000000000);

	time_t now = time(NULL); // Used to calculate elapsed time

	switch(start_option) {
		case SMALL:
			array_length = 1073741824; // 2^30
			M = 1073741824.0;
			filter_probability = 0.25;
			break;

		case LARGE:
			array_length = 2147483648; // 2^31
			M = 2147483648.0;
			filter_probability = 0.50;
			break;

		case HUGE:
			array_length = 4294967296ull; // 2^32
			M = 4294967296.0;
			filter_probability = 1.0;
			break;
	}
	
	uint64_t mem_available = get_available_memory();
	uint64_t need = array_length * sizeof(uint32_t);
	
	// Check if there is RAM available for array allocation without swap
	if(need > mem_available * 0.7) {
		fatal_error("Required %.1f GB of RAM but available only %.1f GB\n",
                    need / (1024.0 * 1024 * 1024),
                    mem_available / (1024.0 * 1024 * 1024));
	}

	array = (uint32_t*)calloc(array_length, sizeof(uint32_t));

	if(array == NULL) {
		fatal_error("No memory available");
	}

	printf("Mode: %s; Target limits -> Max collisions: ", argv[1]);
	if(max_collisions) {
		printf("%zu", max_collisions);
	}
	else {
		max_collisions = SIZE_MAX;
		printf("infinite");
	}

	printf("; Max data: ");

	if(max_bytes) {
		printf("%.2f TB\n\n", max_TB);
	}
	else {
		max_bytes = UINT64_MAX;
		max_TB = DBL_MAX;
		printf("infinite\n\n");
	}

	// Main loop: read blocks of BUF_SIZE 64-bit numbers
	while((n = fread(buffer, sizeof(uint64_t), BUF_SIZE, stdin)) > 0) {
		for(size_t i = 0; i < n; i++) {
			size_t index = (uint32_t)buffer[i];
			if(index < array_length) {
				uint32_t value = (uint32_t)(buffer[i] >> 32);
				if(value) {
					filtered_count++;
					if(array[index] == 0) {
						array[index] = value;
					}
					else if(array[index] == value) {
						printf("--------> Collision: %" PRIu64 "\n", buffer[i]);
						colls++;

						check_statistical_anomaly(filtered_count, colls);

						// SUCCESS TERMINATION 1: Target collisions reached with stable PRNG behavior
						if (colls >= max_collisions) {
							printf("\nSUCCESS: Target of %zu collisions reached\n", max_collisions);
							goto program_end;
						}
					}
				}
			}
		}
		k += n;

		if((++r & 0xFFFF) == 0) {

			// SUCCESS TERMINATION 2: Target data size successfully processed without anomalies
			if(k >= (max_bytes / 8)) {
				printf("\nSUCCESS: Target data volume of %.2f TB processed\n", max_TB);
				goto program_end;
			}

			// 1. Validate if the LSB distribution is uniform (25% pass rate)
			check_filter_anomaly(k, filtered_count);

			// 2. Validate global collisions via Poisson
			double expected_colls = check_statistical_anomaly(filtered_count, colls);

			time_t elapsed = time(NULL) - now;

			printf("Elapsed time: %s; Processed: ", time_interval(elapsed));

			double gb = k * 8e-9; // GB processed
			if(gb < 1000) {
				printf("%.1f GB", gb);
			}
			else {
				double tb = k * 8e-12;
				printf("%.1f TB", tb);
			}
			printf("; Sample size: %" PRIu64 "; Collisions: %zu; Expected: %.1f\n", filtered_count, colls, expected_colls);
		}
	}

program_end:
	// Print final test report before cleanup
	printf("\nFinal Results:\n");
	printf(" - Elapsed time: %s\n", time_interval(time(NULL) - now));
	printf(" - Total data read: %" PRIu64 " items (%.2f GB)\n", k, (k * 8e-9));
	printf(" - Total qualified items (sample size): %" PRIu64 "\n", filtered_count);
	printf(" - Total collisions observed: %zu\n", colls);
	printf(" - Total collisions expected: %.1f\n", collisions_expected(filtered_count));

	array_free();

	return EXIT_SUCCESS;
}

static void fatal_error(const char *frm,...)
{
	char message[1000];
	va_list argptr;

	va_start(argptr, frm);
	vsnprintf(message, sizeof(message), frm, argptr);
	va_end(argptr);

	if(errno) {
		fprintf(stderr, "*** Error: %s: %s\n", message, strerror(errno));
	}
	else {
		fprintf(stderr, "*** Error: %s\n", message);
	}

	array_free();

	exit(EXIT_FAILURE);
}

static void failure(const char *frm,...)
{
	char message[1000];
	va_list argptr;

	va_start(argptr, frm);
	vsnprintf(message, sizeof(message), frm, argptr);
	va_end(argptr);
	fprintf(stderr, "\nFAIL: %s\n", message);

	array_free();

	exit(EXIT_FAILURE);
}

static void copyright(void)
{
	printf("Colsampler v." VERSION " - Copyright(C) 2026 Matteo Zapparoli\n");
	printf("Fast validator of a 64-bit stdin stream collision rate\n\n");
}

static void usage(void)
{
	copyright();
	printf("Usage: colsampler [small | large | huge [<max collisions> [<max TB>]]]\n\n");
	printf("Options (Defaults: <max collisions> = 16, <max TB> = 2.0):\n");
	printf("  <max collisions> : Stop and pass the test after N collisions\n");
	printf("  <max TB>         : Stop and pass the test after X TB processed\n\n");
}

// Return a static string formatting the time interval into hh:mm:ss string
static char *time_interval(time_t t)
{
    static char str[32];

    if (t < 0)
        t = 0;

    uint64_t days = (uint64_t)t / 86400;
    unsigned hh = ((uint64_t)t % 86400) / 3600;
    unsigned mm = ((uint64_t)t % 3600) / 60;
    unsigned ss = (uint64_t)t % 60;

    if (days) {
        snprintf(str, sizeof(str),
                 "%" PRIu64 "d %02u:%02u:%02u",
                 days, hh, mm, ss);
    }
    else {
        snprintf(str, sizeof(str),
                 "%02u:%02u:%02u",
                 hh, mm, ss);
    }

    return str;
}

static void array_free(void)
{
	if(array != NULL)
		free(array);

	array = NULL;
}

static double collisions_expected(uint64_t filtered_count)
{
	double N = (double)filtered_count;

	// Exact FCFS mathematical formula for expected collisions (Lambda)
	// using expm1() for improved numerical stability
	return (1.0 / MSB_VALUES) * (N + M * expm1(-N / M));
}

// Scientific validation function checking statistical anomalies via Poisson distribution
// return the number of expected collisions
static double check_statistical_anomaly(uint64_t filtered_count, size_t real_colls)
{
	double expected = collisions_expected(filtered_count);

	if (expected >= 1.0) {
		// Calculate standard deviation (Square root of Lambda)
		double sigma = sqrt(expected);

		// Confidence threshold set to 4 Sigma (99.9937% statistical coverage)
		double upper_bound = expected + (4.0 * sigma);
		double lower_bound = expected - (4.0 * sigma);
		if (lower_bound < 0) lower_bound = 0;

		// Evaluate if the real stream significantly deviates from ideal randomness
		if ((double)real_colls > upper_bound) {
			failure("Too many collisions! Detected: %zu, Max expected: %.2f (Mean expected: %.2f)",
					real_colls, upper_bound, expected);
		}
		else if ((double)real_colls < lower_bound) {
			failure("Too few collisions! Detected: %zu, Min expected: %.2f (Mean expected: %.2f)",
					real_colls, lower_bound, expected);
		}
	}

	return expected;
}

// Scientific validation of the filter pass-rate using 5-Sigma Binomial/Normal distribution
static void check_filter_anomaly(uint64_t total_read, uint64_t filtered_count)
{
	// Skip validation on HUGE mode or during the early warm-up phase
	if(start_option == HUGE || total_read < 4000000) return;

	double k = (double)total_read;

	// Expected number of values passing the filter
	double expected_mean = k * filter_probability;

	// Binomial standard deviation
	double sigma = sqrt(k * filter_probability * (1.0 - filter_probability));

	// 5-Sigma confidence interval
	double upper_bound = expected_mean + (5.0 * sigma);
	double lower_bound = expected_mean - (5.0 * sigma);

	double actual = (double)filtered_count;

	if(actual > upper_bound || actual < lower_bound) {
		failure("Anomaly distribution detected! "
		        "Processed: %" PRIu64
		        "; Filtered: %" PRIu64
		        " (Expected: %.2f; Allowed Range: %.2f to %.2f)",
		        total_read, filtered_count, expected_mean, lower_bound, upper_bound);
	}
}

// Return 1 id str a size_t, 0 else
static int is_valid_size_t(const char *str, size_t *out_value)
{
	if(str == NULL || *str == '\0' || !isdigit((unsigned char)*str))
		return 0;

	errno = 0;
	char *end;
	unsigned long long v = strtoull(str, &end, 10);

	if(*end != '\0') return 0;
	if(errno == ERANGE || v > SIZE_MAX) return 0;
	if(out_value) *out_value = (size_t)v;

	return 1;
}

static int is_valid_double(const char *str, double *out_value)
{
	errno = 0;
	char *end;

	double d = strtod(str, &end);

	if(*end != '\0' || errno == ERANGE || d < 0.0) return 0;
	if(out_value) *out_value = d;

	return 1;
}

static uint64_t get_available_memory(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return 0;

    char line[256];
    size_t avail_kb = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemAvailable: %zu kB", &avail_kb) == 1)
            break;
    }

    fclose(fp);

    return avail_kb * 1024ULL;
}