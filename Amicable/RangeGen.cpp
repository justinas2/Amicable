#include "stdafx.h"
#include "PrimeTables.h"
#include "Engine.h"
#include "RangeGen.h"
#include "sprp64.h"
#include <sstream>

PRAGMA_WARNING(push, 1)
PRAGMA_WARNING(disable : 4091 4917)
#include <boinc_api.h>
PRAGMA_WARNING(pop)

CRITICAL_SECTION RangeGen::lock = CRITICAL_SECTION_INITIALIZER;
CACHE_ALIGNED RangeGen::StackFrame RangeGen::search_stack[MaxPrimeFactors];
CACHE_ALIGNED Factor RangeGen::factors[MaxPrimeFactors];
int RangeGen::search_stack_depth;
int RangeGen::prev_search_stack_depth;
unsigned int RangeGen::cur_largest_prime_power;
volatile number RangeGen::SharedCounterForSearch;
number RangeGen::total_numbers_checked;
double RangeGen::total_numbers_to_check = 2.5e11;
volatile bool RangeGen::allDone = false;

const char* const CHECKPOINT_LOGICAL_NAME = "amicable_checkpoint";

RangeGen RangeGen::RangeGen_instance;

static FORCEINLINE bool is_abundant_q(const number sum, const number sum_q, const number value_to_check)
{
	// sum * sum_q can be >= 2^64 when SearchLimit::value is large enough, so use 128-bit arithmetic here
	number s2[2];
	s2[0] = _umul128(sum, sum_q, &s2[1]);
	sub128(s2[0], s2[1], value_to_check, 0, s2, s2 + 1);
	return (s2[0] > value_to_check) || s2[1];
}

static FORCEINLINE bool whole_branch_deficient(number value, number sum, const Factor* f)
{
	if (sum - value >= value)
	{
		return false;
	}

	number sumHi = 0;
	number value1 = value;
	number p = f->p;
	const byte* shift = NextPrimeShifts + f->index * 2;
	for (;;)
	{
		p += *shift * ShiftMultiplier;
		shift += 2;
		number h;
		value = _umul128(value, p, &h);
		if ((value >= SearchLimit::value) || h)
		{
			break;
		}

		// sigma(p^k) / p^k =
		// (p^(k+1) - 1) / (p^k * (p - 1)) = 
		// (p - p^-k) / (p-1) <
		// p / (p-1)
		value1 *= p - 1;
		sum = _umul128(sum, p, &sumHi);
	}
	sub128(sum, sumHi, value1, 0, &sum, &sumHi);

	return ((sum < value1) && !sumHi);
}

template<unsigned int largest_prime_power>
NOINLINE bool RangeGen::Iterate(RangeData& range)
{
	StackFrame* s = search_stack + search_stack_depth;
	Factor* f = factors + search_stack_depth;

	int start_i, start_j;
	number q0, q, sum_q;

#define RECURSE ++search_stack_depth; ++s; ++f; goto recurse_begin
#define RETURN --search_stack_depth; --s; --f; goto recurse_begin

recurse_begin:
	const bool is_return = (search_stack_depth < prev_search_stack_depth);
	prev_search_stack_depth = search_stack_depth;
	if (is_return)
	{
		if (search_stack_depth < 0)
		{
			return false;
		}
		goto recurse_return;
	}

	start_i = (search_stack_depth == 0) ? 0 : (factors[search_stack_depth - 1].index + 1);

	f->p = (search_stack_depth == 0) ? 2 : (factors[search_stack_depth - 1].p + NextPrimeShifts[factors[search_stack_depth - 1].index * 2] * ShiftMultiplier);

	// A check to ensure that m is not divisible by 6
	if (search_stack_depth == 1)
	{
		// factors[0].p is 2
		// factors[1].p is 3
		// change factors[1].p to 5
		if (start_i == 1)
		{
			start_i = 2;
			f->p = 5;
		}
	}

	for (f->index = start_i; f->p <= SearchLimit::PrimeInversesBound; f->p += NextPrimeShifts[f->index * 2] * ShiftMultiplier, ++f->index)
	{
		number h;
		s[1].value = _umul128(s->value, f->p, &h);
		if ((s[1].value >= SearchLimit::value) || h)
		{
			RETURN;
		}
		s[1].sum = s->sum * (f->p + 1);

		f->k = 1;

		f->q_max = number(-1) / f->p;

		PRAGMA_WARNING(suppress : 4146)
		f->p_inv = -modular_inverse64(f->p);

		for (;;)
		{
			start_j = f->index + 1;
			q0 = f->p + NextPrimeShifts[f->index * 2] * ShiftMultiplier;

			// A check to ensure that m*q is not divisible by 6
			if (search_stack_depth == 0)
			{
				// factors[0].p is 2
				// q is 3
				// change q to 5
				if (start_j == 1)
				{
					start_j = 2;
					q0 = 5;
				}
			}

			{
				q = q0;
				sum_q = q0 + 1;

				IF_CONSTEXPR(largest_prime_power > 1)
				{
					q = _umul128(q, q0, &h);
					if (h)
					{
						if (f->k == 1)
						{
							RETURN;
						}
						break;
					}
					sum_q += q;
				}

				IF_CONSTEXPR(largest_prime_power > 2)
				{
					q = _umul128(q, q0, &h);
					if (h)
					{
						if (f->k == 1)
						{
							RETURN;
						}
						break;
					}
					sum_q += q;
				}

				// We don't need to check if sum_q fits in 64 bits because q < SearchLimit::value / s[1].value,
				// where s[1].value must be >= 20 for s[1].value * q to be amicable number

				const number value_to_check = _umul128(s[1].value, q, &h);
				if ((value_to_check >= SearchLimit::value) || h)
				{
					if (f->k == 1)
					{
						RETURN;
					}
					break;
				}

				// Skip overabundant numbers
				const bool is_deficient = (s[1].sum - s[1].value < s[1].value);
				if (is_deficient || !OverAbundant<(largest_prime_power & 1) ? 2 : 1>(factors, search_stack_depth, s[1].value, s[1].sum, (largest_prime_power & 1) ? 2 : 1))
				{
					if (!is_deficient || is_abundant_q(s[1].sum, sum_q, value_to_check))
					{
						range.value = s[1].value;
						range.sum = s[1].sum;
						range.start_prime = q0;
						range.index_start_prime = static_cast<unsigned int>(start_j);
						for (unsigned int i = 0; i <= static_cast<unsigned int>(search_stack_depth); ++i)
						{
							range.factors[i] = factors[i];
						}
						for (unsigned int i = static_cast<unsigned int>(search_stack_depth) + 1; i < MaxPrimeFactors; ++i)
						{
							range.factors[i].p = 0;
							range.factors[i].k = 0;
						}
						range.last_factor_index = search_stack_depth;
						++search_stack_depth;
						return true;
					}

					if (!whole_branch_deficient(s[1].value, s[1].sum, f))
					{
						RECURSE;
					}
				}
			}

recurse_return:
			s[1].value = _umul128(s[1].value, f->p, &h);
			if ((s[1].value >= SearchLimit::value) || h)
			{
				break;
			}
			s[1].sum = s[1].sum * f->p + s->sum;
			++f->k;
		}

		// Workaround for shifting from 2 to 3 because NextPrimeShifts[0] is 0
		if (search_stack_depth == 0)
		{
			if (f->p == 2)
			{
				f->p = 3;
			}
			// Check only 2, 3, 5 as the smallest prime factor because the smallest abundant number coprime to 2*3*5 is ~2*10^25
			if (f->p >= 5)
			{
				break;
			}
		}
	}
	if (search_stack_depth > 0)
	{
		RETURN;
	}
	search_stack_depth = -1;
	return false;
}

bool RangeGen::Iterate(RangeData& range)
{
	if (search_stack_depth < 0)
	{
		return false;
	}

	if (cur_largest_prime_power == 1)
	{
		if (Iterate<1>(range))
		{
			return true;
		}
		else
		{
			cur_largest_prime_power = 2;
			search_stack_depth = 0;
			prev_search_stack_depth = 0;
		}
	}

	if (cur_largest_prime_power == 2)
	{
		if (Iterate<2>(range))
		{
			return true;
		}
		else
		{
			cur_largest_prime_power = 3;
			search_stack_depth = 0;
			prev_search_stack_depth = 0;
		}
	}

	if (cur_largest_prime_power == 3)
	{
		return Iterate<3>(range);
	}

	return false;
}

template<typename T>
FORCEINLINE unsigned int ParseFactorization(char* factorization, T callback)
{
	unsigned int numFactors = 0;
	int counter = 0;
	number prev_p = 0;
	number p = 0;
	number p1 = 2;
	int index_p1 = 0;
	unsigned int k = 0;
	for (char* ptr = factorization, *prevPtr = factorization; ; ++ptr)
	{
		const char c = *ptr;
		if ((c == '^') || (c == '*') || (c == '\0'))
		{
			*ptr = '\0';
			++counter;
			if (counter == 1)
			{
				p = static_cast<number>(StrToNumber(prevPtr));
			}
			else if (counter == 2)
			{
				k = static_cast<unsigned int>(atoi(prevPtr));
			}

			if (((counter == 1) && (c != '^')) || (counter == 2))
			{
				if (counter == 1)
				{
					k = 1;
				}
				counter = 0;
				if ((numFactors > 0) && (p <= prev_p))
				{
					std::cerr << "Factorization '" << factorization << "' is incorrect: factors must be in increasing order" << std::endl;
					abort();
				}
				prev_p = p;

				if (p1 < p)
				{
					if (p1 == 2)
					{
						p1 = 3;
						++index_p1;
					}
					while (p1 < p)
					{
						p1 += NextPrimeShifts[index_p1 * 2] * ShiftMultiplier;
						++index_p1;
					}
				}

				if (p1 != p)
				{
					std::cerr << "Factorization '" << factorization << "' is incorrect: " << p << " is not a prime" << std::endl;
					abort();
				}

				if (k < 1)
				{
					std::cerr << "Factorization '" << factorization << "'is incorrect: prime power for " << p << " must be >= 1" << std::endl;
					abort();
				}

				callback(p, k, index_p1, numFactors);

				++numFactors;

				if (numFactors >= MaxPrimeFactors)
				{
					std::cerr << "Factorization '" << factorization << "' is incorrect: too many prime factors" << std::endl;
					abort();
				}
			}
			*ptr = c;
			prevPtr = ptr + 1;
			if (c == '\0')
			{
				break;
			}
		}
	}

	if (numFactors == 0)
	{
		std::cerr << "Factorization '" << factorization << "' is incorrect: too few prime factors" << std::endl;
		abort();
	}

	return numFactors;
}

NOINLINE void RangeGen::Init(char* startFrom, char* stopAt, RangeData* outStartFromRange, Factor* outStopAtFactors, unsigned int largestPrimePower)
{
	search_stack[0].value = 1;
	search_stack[0].sum = 1;
	search_stack_depth = 0;
	prev_search_stack_depth = 0;
	cur_largest_prime_power = largestPrimePower;
	SharedCounterForSearch = 0;

	if (startFrom)
	{
		const unsigned int numFactors = ParseFactorization(startFrom,
			[startFrom](number p, unsigned int k, int p_index, unsigned int factor_index)
			{
				Factor& f = factors[factor_index];
				f.p = p;
				f.k = k;
				f.index = p_index;

				f.q_max = number(-1) / p;

				PRAGMA_WARNING(suppress : 4146)
				f.p_inv = -modular_inverse64(p);

				if ((f.p > 2) && (f.p * f.p_inv != 1))
				{
					std::cerr << "Internal error: modular_inverse64 table is incorrect" << std::endl;
					abort();
				}

				StackFrame& cur_s = search_stack[factor_index];
				StackFrame& next_s = search_stack[factor_index + 1];
				next_s.value = cur_s.value;
				next_s.sum = cur_s.sum;
				for (number i = 0; i < k; ++i)
				{
					number h;
					next_s.value = _umul128(next_s.value, p, &h);
					if ((next_s.value >= SearchLimit::value) || h)
					{
						std::cerr << "Factorization '" << startFrom << "' is incorrect: number is too large" << std::endl;
						abort();
					}
					next_s.sum = next_s.sum * p + cur_s.sum;
				}
			}
		);

		search_stack_depth = static_cast<int>(numFactors);
		prev_search_stack_depth = static_cast<int>(numFactors);

		StackFrame* s = search_stack + search_stack_depth;
		Factor* f = factors + search_stack_depth - 1;
		RangeData& range = *outStartFromRange;
		range.value = 0;

		int start_j = f->index + 1;
		number q0 = f->p + NextPrimeShifts[f->index * 2] * ShiftMultiplier;

		// A check to ensure that m*q is not divisible by 6
		if (search_stack_depth == 1)
		{
			// factors[0].p is 2
			// q is 3
			// change q to 5
			if (start_j == 1)
			{
				start_j = 2;
				q0 = 5;
			}
		}

		number q = q0;
		number sum_q = q0 + 1;

		number h;
		const number value_to_check = _umul128(s->value, q, &h);
		if ((value_to_check < SearchLimit::value) && !h)
		{
			// Skip overabundant numbers
			const bool is_deficient = (s->sum - s->value < s->value);
			if (is_deficient || !OverAbundant<2>(factors, static_cast<int>(numFactors) - 1, s->value, s->sum, static_cast<number>((cur_largest_prime_power & 1) ? 2 : 1)))
			{
				if (!is_deficient || is_abundant_q(s->sum, sum_q, value_to_check))
				{
					range.value = s->value;
					range.sum = s->sum;
					range.start_prime = q0;
					range.index_start_prime = static_cast<unsigned int>(start_j);
					memcpy(range.factors, factors, sizeof(Factor) * numFactors);
					range.last_factor_index = static_cast<int>(numFactors) - 1;
				}
			}
		}
	}

	if (stopAt)
	{
		ParseFactorization(stopAt,
			[outStopAtFactors](number p, unsigned int k, int /*p_index*/, unsigned int factor_index)
			{
				outStopAtFactors[factor_index].p = p;
				outStopAtFactors[factor_index].k = k;
			}
		);
	}
}

NOINLINE void RangeGen::Run(number numThreads, char* startFrom, char* stopAt, unsigned int largestPrimePower, number startPrime, number primeLimit)
{
	RangeData startFromRange;
	Factor stopAtFactors[MaxPrimeFactors + 1] = {};

	char checkpoint_buf[256];
	std::string checkpoint_name;
	boinc_resolve_filename_s(CHECKPOINT_LOGICAL_NAME, checkpoint_name);
	FILE* checkpoint = boinc_fopen(checkpoint_name.c_str(), "r");
	if (checkpoint)
	{
		if (fgets(checkpoint_buf, sizeof(checkpoint_buf), checkpoint))
		{
			char* end_of_data = strchr(checkpoint_buf, '#');
			if (end_of_data)
			{
				*end_of_data = '\0';
				char* first_token_end = strchr(checkpoint_buf, ' ');
				if (first_token_end)
				{
					total_numbers_checked = StrToNumber(checkpoint_buf);
					const double f = total_numbers_checked / total_numbers_to_check;
					boinc_fraction_done((f > 1.0) ? 1.0 : f);
					do
					{
						++first_token_end;
					} while (*first_token_end == ' ');
					startFrom = first_token_end;
				}
			}
		}
		fclose(checkpoint);
	}

	Init(startFrom, stopAt, &startFromRange, stopAtFactors, largestPrimePower);

	if (numThreads == 0)
	{
		numThreads = std::thread::hardware_concurrency();
	}

	if ((numThreads == 0) || (numThreads > 2048))
	{
		numThreads = 1;
	}

	std::vector<std::thread> threads;

	WorkerThreadParams* params = reinterpret_cast<WorkerThreadParams*>(alloca(sizeof(WorkerThreadParams) * numThreads));
	for (number i = 0; i < numThreads; ++i)
	{
		params[i].rangeToCheckFirst = (startFrom && startFromRange.value && (i == 0)) ? &startFromRange : nullptr;
		params[i].stopAtFactors = stopAt ? stopAtFactors : nullptr;
		params[i].startPrime = startPrime;
		params[i].primeLimit = primeLimit;
		params[i].startLargestPrimePower = largestPrimePower;
		params[i].finished = false;
		threads.emplace_back(std::thread(WorkerThread, &params[i]));
	}

	std::thread checkpoint_thread(CheckpointThread, params, numThreads);

	for (number i = 0; i < numThreads; ++i)
	{
		threads[i].join();
	}

	allDone = true;
	checkpoint_thread.join();
}

NOINLINE void RangeGen::CheckpointThread(WorkerThreadParams* params, number numWorkerThreads)
{
	std::string checkpoint_name, data;
	boinc_resolve_filename_s(CHECKPOINT_LOGICAL_NAME, checkpoint_name);

	while (!allDone)
	{
		Sleep(1000);
		if (boinc_time_to_checkpoint())
		{
			WorkerThreadState mostLaggingThreadState = {};
			mostLaggingThreadState.curLargestPrimePower = std::numeric_limits<unsigned int>::max();

			// Find the most lagging thread and use this thread's state as current checkpoint
			EnterCriticalSection(&lock);

			for (number i = 0; i < numWorkerThreads; ++i)
			{
				if (!params[i].finished)
				{
					bool is_less = false;

					if (params[i].stateToSave.curLargestPrimePower < mostLaggingThreadState.curLargestPrimePower)
					{
						is_less = true;
					}
					else if (params[i].stateToSave.curLargestPrimePower == mostLaggingThreadState.curLargestPrimePower)
					{
						const Factor (&mainFactors)[MaxPrimeFactors] = mostLaggingThreadState.curRange.factors;
						const Factor (&curFactors)[MaxPrimeFactors] = params[i].stateToSave.curRange.factors;

						number j;
						for (j = 0; j <= static_cast<number>(params[i].stateToSave.curRange.last_factor_index); ++j)
						{
							if (curFactors[j].p != mainFactors[j].p)
							{
								is_less = (curFactors[j].p < mainFactors[j].p);
								break;
							}
							else if (curFactors[j].k != mainFactors[j].k)
							{
								is_less = (curFactors[j].k < mainFactors[j].k);
								break;
							}
						}

						if (j > static_cast<number>(params[i].stateToSave.curRange.last_factor_index))
						{
							is_less = (params[i].stateToSave.curRange.last_factor_index < mostLaggingThreadState.curRange.last_factor_index);
						}
					}

					if (is_less)
					{
						mostLaggingThreadState = params[i].stateToSave;
					}
				}
			}

			LeaveCriticalSection(&lock);

			std::stringstream s;

			// First write how many numbers have been checked so far
			s << mostLaggingThreadState.total_numbers_checked << ' ';

			// Then factorization from most lagging thread
			Factor (&mainFactors)[MaxPrimeFactors] = mostLaggingThreadState.curRange.factors;
			for (number i = 0; i <= static_cast<number>(mostLaggingThreadState.curRange.last_factor_index); ++i)
			{
				if (i > 0)
				{
					s << '*';
				}
				s << mainFactors[i].p;
				if (mainFactors[i].k > 1)
				{
					s << '^' << mainFactors[i].k;
				}
			}

			// End of data marker
			s << '#';

			data = s.str();

			FILE* f = boinc_fopen(checkpoint_name.c_str(), "wb");
			if (f)
			{
				fwrite(data.c_str(), sizeof(char), data.length(), f);
				fclose(f);
			}

			boinc_checkpoint_completed();
		}
	}
}

NOINLINE void RangeGen::WorkerThread(WorkerThreadParams* params)
{
	SetLowPriority();
	ForceRoundUpFloatingPoint();

	number numbers_checked = 0;

	if (params->startPrime && params->primeLimit)
	{
		if (params->startPrime < SearchLimit::MainPrimeTableBound + 1)
		{
			params->startPrime = SearchLimit::MainPrimeTableBound + 1;
		}
		if (params->primeLimit > SearchLimit::SafeLimit)
		{
			params->primeLimit = SearchLimit::SafeLimit;
		}
		if (params->primeLimit < params->startPrime)
		{
			params->primeLimit = params->startPrime;
		}
		SearchLargePrimes(&SharedCounterForSearch, params->startPrime, params->primeLimit);
	}
	else
	{
		if (params->rangeToCheckFirst)
		{
			switch (params->startLargestPrimePower)
			{
			case 1:
				numbers_checked += SearchRange(*params->rangeToCheckFirst);
				break;
			case 2:
				numbers_checked += SearchRangeSquared(*params->rangeToCheckFirst);
				break;
			case 3:
				numbers_checked += SearchRangeCubed(*params->rangeToCheckFirst);
				break;
			}
		}

		enum
		{
			RangesReadAheadSize = 4,
		};
		RangeData ranges[RangesReadAheadSize];
		unsigned int range_largest_prime_power[RangesReadAheadSize];
		number range_write_index = 0;
		number range_read_index = 0;
		for (;;)
		{
			bool is_locked = TryEnterCriticalSection(&lock) != 0;
			if (!is_locked && (range_read_index == range_write_index))
			{
				EnterCriticalSection(&lock);
				is_locked = true;
			}

			if (is_locked)
			{
				if (numbers_checked)
				{
					total_numbers_checked += numbers_checked;
					numbers_checked = 0;

					const double f = total_numbers_checked / total_numbers_to_check;
					boinc_fraction_done((f > 1.0) ? 1.0 : f);
				}

				while (range_write_index - range_read_index < RangesReadAheadSize)
				{
					RangeData& r = ranges[range_write_index % RangesReadAheadSize];
					if (!Iterate(r))
					{
						break;
					}
					range_largest_prime_power[range_write_index % RangesReadAheadSize] = cur_largest_prime_power;

					if (params->stopAtFactors)
					{
						if (cur_largest_prime_power != params->startLargestPrimePower)
						{
							search_stack_depth = -1;
							break;
						}

						bool done = false;
						number i;
						for (i = 0; i < MaxPrimeFactors; ++i)
						{
							if (r.factors[i].p != params->stopAtFactors[i].p)
							{
								done = (r.factors[i].p > params->stopAtFactors[i].p);
								break;
							}
							else if (r.factors[i].k != params->stopAtFactors[i].k)
							{
								done = (r.factors[i].k > params->stopAtFactors[i].k);
								break;
							}
						}

						if (done || (i == MaxPrimeFactors))
						{
							search_stack_depth = -1;
							break;
						}
					}

					++range_write_index;
				}

				if (range_read_index != range_write_index)
				{
					params->stateToSave.curRange = ranges[range_read_index % RangesReadAheadSize];
					params->stateToSave.curLargestPrimePower = range_largest_prime_power[range_read_index % RangesReadAheadSize];
					params->stateToSave.total_numbers_checked = total_numbers_checked;
				}

				LeaveCriticalSection(&lock);
			}

			if (range_read_index == range_write_index)
			{
				break;
			}

			switch (range_largest_prime_power[range_read_index % RangesReadAheadSize])
			{
			case 1:
				numbers_checked += SearchRange(ranges[range_read_index % RangesReadAheadSize]);
				break;
			case 2:
				numbers_checked += SearchRangeSquared(ranges[range_read_index % RangesReadAheadSize]);
				break;
			case 3:
				numbers_checked += SearchRangeCubed(ranges[range_read_index % RangesReadAheadSize]);
				break;
			}
			++range_read_index;
		}

		if (!params->stopAtFactors)
		{
			SearchLargePrimes(&SharedCounterForSearch, SearchLimit::MainPrimeTableBound + 1, SearchLimit::SafeLimit);
		}
	}

	params->finished = true;
}
