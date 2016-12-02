///
/// @file  PrimeFinder.hpp
///
/// Copyright (C) 2016 Kim Walisch, <kim.walisch@gmail.com>
///
/// This file is distributed under the BSD License. See the COPYING
/// file in the top level directory.
///

#ifndef PRIMEFINDER_HPP
#define PRIMEFINDER_HPP

#include "config.hpp"
#include "SieveOfEratosthenes.hpp"

#include <stdint.h>
#include <vector>

namespace primesieve {

class PrimeSieve;
class PreSieve;

/// PrimeFinder is a SieveOfEratosthenes class that is used to
/// callback, print and count primes and prime k-tuplets
/// (twin primes, prime triplets, ...).
///
class PrimeFinder : public SieveOfEratosthenes {
public:
  PrimeFinder(PrimeSieve&, const PreSieve&);
private:
  enum { END = 0xff + 1 };
  static const uint_t kBitmasks_[6][5];
  /// Count lookup tables for prime k-tuplets
  std::vector<uint_t> kCounts_[6];
  /// Reference to the associated PrimeSieve object
  PrimeSieve& ps_;
  void init_kCounts();
  virtual void segmentFinished(const byte_t*, uint_t);
  void count(const byte_t*, uint_t);
  void print(const byte_t*, uint_t) const;
  template <typename T> void callbackPrimes(T, const byte_t*, uint_t) const;
  void callbackPrimes(const byte_t*, uint_t) const;
  static void printPrime(uint64_t);
  DISALLOW_COPY_AND_ASSIGN(PrimeFinder);
};

template<typename T>
class PrimeFinderTemplated : public PrimeFinder
{
public:
	PrimeFinderTemplated(PrimeSieve& ps, const PreSieve& preSieve, T&& callback) : PrimeFinder(ps, preSieve), myCallback(callback) {}

private:
	T& myCallback;

	virtual void segmentFinished(const byte_t* sieve, uint_t sieveSize)
	{
		uint64_t base = getSegmentLow();
		for (uint_t i = 0; i < sieveSize; i += 8, base += NUMBERS_PER_BYTE * 8)
		{
			uint64_t bits = littleendian_cast<uint64_t>(&sieve[i]); 
			while (bits != 0)
			{
				myCallback(getNextPrime(&bits, base));
			}
		}
	}

	DISALLOW_COPY_AND_ASSIGN(PrimeFinderTemplated);
};

} // namespace

#endif
