# Colsampler

Fast collision-rate validator for 64-bit PRNG streams.

Colsampler is a standalone command-line tool that evaluates a 64-bit random stream by measuring exact 64-bit collisions on a deterministic sample of the input data.

Unlike test libraries, Colsampler does not need to be linked or integrated into a PRNG implementation. Any generator capable of producing a binary `uint64_t` stream can be tested directly through standard input.

## Features

- Works with any source that produces a binary `uint64_t` stream.
- No integration or recompilation required.
- Exact 64-bit collision detection.
- Constant-time lookup and update operations.
- No sorting.
- No hashing.
- No external dependencies.
- Statistical validation based on expected Poisson collision rates.
- Designed to process terabytes of data efficiently.

## How It Works

For each 64-bit value:

- The lower 32 bits are used as an index.
- The upper 32 bits are stored as the associated value.

The first value reaching a given index becomes permanently anchored to that slot.

Subsequent values:

- generate a collision if the stored value matches;
- are ignored if the stored value differs.

This First-Come-First-Served (FCFS) approach allows exact collision detection while keeping memory access and processing cost constant.

## Why It Is Fast

Many collision-analysis tools require:

- sorting large datasets;
- storing all samples;
- performing expensive searches.

Colsampler uses a direct-addressing table instead.

As a result:

- memory usage is fixed;
- each sample is processed in **O(1)** time;
- execution speed is mainly limited by memory bandwidth.

## Sampling Instead of Exhaustive Storage

Colsampler does not store every 64-bit value received.

Instead, it monitors a deterministic subset of the stream selected through the lower 32 bits.

The resulting collision count is therefore measured on a sample rather than on the entire stream.

For uniformly distributed random data, this sample remains statistically representative, allowing reliable comparison between observed and theoretically expected collision counts.

## Execution Modes

| Mode | Table Size | RAM |
|--------|--------|--------|
| small | 2³⁰ slots | 4 GiB |
| large | 2³¹ slots | 8 GiB |
| huge | 2³² slots | 16 GiB |

## Build

```bash
gcc -O3 colsampler.c -o colsampler -lm
```

## Usage

```bash
colsampler [small | large | huge [max_collisions [max_TB]]]
```

### Examples

```bash
./my_prng | colsampler small
```

```bash
./my_prng | colsampler large 32
```

```bash
./my_prng | colsampler huge 64 10
```

## Example Output

```text
Elapsed time: 00:12:44; Processed: 1.0 TB;
Sample size: 134233321;
Collisions: 2;
Expected: 1.8
```

A healthy random stream should produce a collision count close to the theoretical expectation.

Colsampler automatically reports statistically significant excesses or deficits of collisions.

## Intended Use

Colsampler is not a replacement for batteries such as:

- PractRand
- TestU01
- Dieharder

Instead, it is a complementary tool focused on one specific property:

> Does the stream produce the expected number of exact 64-bit collisions?

This makes it particularly useful for long-running validation campaigns where trillions of values must be processed with minimal overhead.

## License

Apache License 2.0
