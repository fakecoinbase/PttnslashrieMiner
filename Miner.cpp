/* (c) 2014-2017 dave-andersen (base code) (http://www.cs.cmu.edu/~dga/)
(c) 2017-2020 Pttn (refactoring and porting to modern C++) (https://github.com/Pttn/rieMiner)
(c) 2018 Michael Bell/Rockhawk (assembly optimizations, improvements of work management between threads, and some more) (https://github.com/MichaelBell/) */

#include <gmpxx.h> // With Uint64_Ts, we still need to use the Mpz_ functions, otherwise there are "ambiguous overload" errors on Windows...

#include "external/gmp_util.h"
#include "ispc/fermat.h"
#include "Miner.hpp"

thread_local bool isMaster(false);
thread_local uint64_t** offsetStack(NULL);
thread_local uint64_t** offsetCount(NULL);

#define MAX_SIEVE_WORKERS 16
#define NUM_PRIMES_TO_2P32 203280222
#define	ZEROS_BEFORE_HASH	8

extern "C" {
	void rie_mod_1s_4p_cps(uint64_t *cps, uint64_t p);
	mp_limb_t rie_mod_1s_4p(mp_srcptr ap, mp_size_t n, uint64_t ps, uint64_t cnt, uint64_t* cps);
	mp_limb_t rie_mod_1s_2p_4times(mp_srcptr ap, mp_size_t n, uint32_t* ps, uint32_t cnt, uint64_t* cps, uint64_t* remainders);
	mp_limb_t rie_mod_1s_2p_8times(mp_srcptr ap, mp_size_t n, uint32_t* ps, uint32_t cnt, uint64_t* cps, uint64_t* remainders);
}

static const mpz_class mpz2(2);
bool isPrimeFermat(const mpz_class& n) {
	mpz_class r, nm1(n - 1);
	mpz_powm(r.get_mpz_t(), mpz2.get_mpz_t(), nm1.get_mpz_t(), n.get_mpz_t());
	return r == 1;
}

void Miner::init() {
	_parameters.threads = _manager->options().threads();
	_parameters.primorialOffsets = v64ToVMpz(_manager->options().primorialOffsets());
	_parameters.sieveWorkers = _manager->options().sieveWorkers();
	if (_parameters.sieveWorkers == 0) {
		_parameters.sieveWorkers = std::max(_manager->options().threads()/5, 1);
		_parameters.sieveWorkers += (_manager->options().primeTableLimit() + 0x80000000ull) >> 33;
	}
	_parameters.sieveWorkers = std::min(_parameters.sieveWorkers, MAX_SIEVE_WORKERS);
	_parameters.sieveWorkers = std::min(_parameters.sieveWorkers, int(_parameters.primorialOffsets.size()));
	std::cout << "Sieve Workers = " << _parameters.sieveWorkers << std::endl;
	std::cout << "Best SIMD instructions supported:";
	if (_cpuInfo.hasAVX512()) std::cout << " AVX-512";
	else if (_cpuInfo.hasAVX2()) {
		std::cout << " AVX2";
		if (!_manager->options().enableAvx2()) std::cout << " (disabled -> AVX)";
	}
	else if (_cpuInfo.hasAVX()) std::cout << " AVX";
	else std::cout << " AVX not suppported!";
	std::cout << std::endl;
	_parameters.sieveBits = _manager->options().sieveBits();
	_parameters.sieveSize = 1 << _parameters.sieveBits;
	_parameters.sieveWords = _parameters.sieveSize/64;
	_parameters.maxIter = _parameters.maxIncrements/_parameters.sieveSize;
	_parameters.solo = !(_manager->options().mode() == "Pool");
	_parameters.tupleLengthMin = _manager->options().tupleLengthMin();
	_parameters.primeTableLimit = _manager->options().primeTableLimit();
	_parameters.primorialNumber  = _manager->options().primorialNumber();
	_parameters.primeTupleOffset = _manager->options().constellationType();
	
	// Empirical formula, should work well in most cases for 6-tuples.
	if (_manager->options().constellationType().size() == 6) {
		double ptlM(((double) _parameters.primeTableLimit)/1048576.), baseMemUsage(1.68*std::pow(ptlM, 0.954)), sieveWorkerMemUsage, memUsage;
		if (ptlM < 768.) sieveWorkerMemUsage = 1.26*ptlM + 16.;
		else sieveWorkerMemUsage = 560.*std::log(ptlM) - 2780.;
		memUsage = baseMemUsage + ((double) _parameters.sieveWorkers)*sieveWorkerMemUsage;
		if (memUsage < 128.) std::cout << "Estimated memory usage: < 128 MiB" << std::endl;
		else std::cout << "Estimated memory usage: " << memUsage << " MiB" << std::endl;
		std::cout << "Reduce prime table limit to lower this, if needed." << std::endl;
	}

	// For larger ranges of offsets, need to add more inverts in _updateRemainders().
	std::transform(_parameters.primeTupleOffset.begin(),
	               _parameters.primeTupleOffset.end(),
	               std::back_inserter(_halfPrimeTupleOffset),
	               [](uint64_t n) {/*assert(n <= 6); */return n >> 1;});
	_primorialOffsetDiff.resize(_parameters.sieveWorkers - 1);
	_primorialOffsetDiffToFirst.resize(_parameters.sieveWorkers);
	_primorialOffsetDiffToFirst[0] = 0;
	const uint64_t tupleSpan(std::accumulate(_parameters.primeTupleOffset.begin(), _parameters.primeTupleOffset.end(), 0));
	for (int j(1) ; j < _parameters.sieveWorkers ; j++) {
		_primorialOffsetDiff[j - 1] = _manager->options().primorialOffsets()[j] - _manager->options().primorialOffsets()[j - 1] - tupleSpan;
		_primorialOffsetDiffToFirst[j] = _manager->options().primorialOffsets()[j] - _manager->options().primorialOffsets()[0];
	}
	
	{
		std::chrono::time_point<std::chrono::system_clock> t0(std::chrono::system_clock::now());
		std::cout << "Generating prime table using sieve of Eratosthenes..." << std::endl;
		std::vector<uint8_t> vfComposite;
		vfComposite.resize((_parameters.primeTableLimit + 15)/16, 0);
		for (uint64_t nFactor(3) ; nFactor*nFactor < _parameters.primeTableLimit ; nFactor += 2) {
			if (vfComposite[nFactor >> 4] & (1 << ((nFactor >> 1) & 7))) continue;
			for (uint64_t nComposite((nFactor*nFactor) >> 1) ; nComposite < (_parameters.primeTableLimit >> 1) ; nComposite += nFactor)
				vfComposite[nComposite >> 3] |= 1 << (nComposite & 7);
		}
		_parameters.primes.push_back(2);
		for (uint64_t n(1) ; (n << 1) + 1 < _parameters.primeTableLimit ; n++) {
			if (!(vfComposite[n >> 3] & (1 << (n & 7))))
				_parameters.primes.push_back((n << 1) + 1);
		}
		_nPrimes = _parameters.primes.size();
		std::cout << "Table with all " << _nPrimes << " first primes generated in " << timeSince(t0) << " s." << std::endl;
	}
	
	mpz_set_ui(_primorial.get_mpz_t(), _parameters.primes[0]);
	for (uint64_t i(1) ; i < _parameters.primorialNumber ; i++)
		mpz_mul_ui(_primorial.get_mpz_t(), _primorial.get_mpz_t(), _parameters.primes[i]);
	std::cout << "Primorial has " << mpz_sizeinbase(_primorial.get_mpz_t(), 2) << " binary digits" << std::endl;
	const uint64_t precompPrimes(std::min(_nPrimes, 5586502348UL)); // Precomputation only works up to p = 2^37
	std::cout << "Precomputing division data..." << std::endl;
	_parameters.inverts.resize(_nPrimes);
	_parameters.modPrecompute.resize(precompPrimes);
	
	_startingPrimeIndex = _parameters.primorialNumber;
	const uint64_t blockSize((_nPrimes - _startingPrimeIndex + _parameters.threads - 1)/_parameters.threads);
	std::thread threads[_parameters.threads];
	for (int16_t j(0) ; j < _parameters.threads ; j++) {
		threads[j] = std::thread([&, j]() {
			mpz_class candidate, prime;
			const uint64_t endIndex(std::min(_startingPrimeIndex + (j + 1)*blockSize, _nPrimes));
			for (uint64_t i(_startingPrimeIndex + j*blockSize) ; i < endIndex ; i++) {
				mpz_set_ui(prime.get_mpz_t(), _parameters.primes[i]);
				mpz_invert(candidate.get_mpz_t(), _primorial.get_mpz_t(), prime.get_mpz_t());
				_parameters.inverts[i] = mpz_get_ui(candidate.get_mpz_t());
				if (i < precompPrimes)
					rie_mod_1s_4p_cps(&_parameters.modPrecompute[i], _parameters.primes[i]);
			}
		});
	}
	for (int16_t j(0) ; j < _parameters.threads ; j++) threads[j].join();
	
	uint64_t highSegmentEntries(0);
	double highFloats(0.), tupleSizeAsDouble(_parameters.primeTupleOffset.size());
	_primeTestStoreOffsetsSize = 0;
	_sparseLimit = 0;
	for (uint64_t i(5) ; i < _nPrimes ; i++) {
		const uint64_t p(_parameters.primes[i]);
		if (p < _parameters.maxIncrements) _primeTestStoreOffsetsSize++;
		else {
			if (_sparseLimit == 0) _sparseLimit = i & (~1ull);
			highFloats += ((tupleSizeAsDouble*_parameters.maxIncrements)/(double) p);
		}
	}
	if (_sparseLimit == 0) {
		_nPrimes &= (~1ull);
		_sparseLimit = _nPrimes;
	}
	
	highSegmentEntries = ceil(highFloats);
	if (highSegmentEntries == 0) _entriesPerSegment = 1;
	else {
		_entriesPerSegment = highSegmentEntries/_parameters.maxIter + 4; // Rounding up a bit
		_entriesPerSegment = (_entriesPerSegment + (_entriesPerSegment >> 3));
	}
	
	try {
		_sieves = new SieveInstance[_parameters.sieveWorkers];
		for (int i(0) ; i < _parameters.sieveWorkers ; i++) {
			_sieves[i].id = i;
			_sieves[i].segmentCounts = new std::atomic<uint64_t>[_parameters.maxIter];
		}

		DBG(std::cout << "Allocating " << _parameters.sieveSize/8*_parameters.sieveWorkers << " bytes for the sieves..." << std::endl;);
		for (int i(0) ; i < _parameters.sieveWorkers ; i++)
			_sieves[i].sieve = new uint8_t[_parameters.sieveSize/8];
	}
	catch (std::bad_alloc& ba) {
		std::cerr << __func__ << ": unable to allocate memory for the miner.sieves :|..." << std::endl;
		exit(-1);
	}

	try {
		DBG(std::cout << "Allocating " << _parameters.primeTupleOffset.size()*4*(_primeTestStoreOffsetsSize + 1024) << " bytes for the offsets..." << std::endl;);
		for (int i(0) ; i < _parameters.sieveWorkers ; i++)
			_sieves[i].offsets = new uint32_t[(_primeTestStoreOffsetsSize + 1024)*_parameters.primeTupleOffset.size()];
	}
	catch (std::bad_alloc& ba) {
		std::cerr << __func__ << ": unable to allocate memory for the offsets :|..." << std::endl;
		exit(-1);
	}

	for (int i(0) ; i < _parameters.sieveWorkers ; i++)
		memset(_sieves[i].offsets, 0, sizeof(uint32_t)*_parameters.primeTupleOffset.size()*(_primeTestStoreOffsetsSize + 1024));

	try {
		DBG(std::cout << "Allocating " << 4*_parameters.maxIter*_entriesPerSegment << " bytes for the segment hits..." << std::endl;);
		for (int i(0) ; i < _parameters.sieveWorkers ; i++) {
			_sieves[i].segmentHits = new uint32_t*[_parameters.maxIter];
			for (uint64_t j(0); j < _parameters.maxIter; j++)
				_sieves[i].segmentHits[j] = new uint32_t[_entriesPerSegment];
		}
	}
	catch (std::bad_alloc& ba) {
		std::cerr << __func__ << ": unable to allocate memory for the segment hits :|..." << std::endl;
		exit(-1);
	}

	// Initial guess at a value for maxWorkOut
	_maxWorkOut = std::min(_parameters.threads*32u*_parameters.sieveWorkers, _workDoneQueue.size() - 256);
	
	_inited = true;
}

void Miner::_putOffsetsInSegments(SieveInstance& sieve, uint64_t *offsets, uint64_t* counts, int n_offsets) {
	for (uint64_t segment(0) ; segment < _parameters.maxIter ; segment++) {
		const uint64_t curSegmentCount(sieve.segmentCounts[segment].fetch_add(counts[segment])),
		               sc(curSegmentCount + counts[segment]);
		if (sc >= _entriesPerSegment) {
			std::cerr << __func__ << ": segment " << segment << " " << sc << " count is > " << _entriesPerSegment << std::endl;
			abort();
		}
		counts[segment] = curSegmentCount;
	}
	for (int i(0) ; i < n_offsets ; i++) {
		const uint64_t index(offsets[i]),
		               segment(index >> _parameters.sieveBits),
		               sc(counts[segment]);
		sieve.segmentHits[segment][sc] = index & (_parameters.sieveSize - 1);
		counts[segment]++;
	}
	for (uint64_t segment(0); segment < _parameters.maxIter; segment++)
		counts[segment] = 0;
}

void Miner::_updateRemainders(uint32_t workDataIndex, uint64_t start_i, uint64_t end_i) {
	mpz_class tar(_workData[workDataIndex].verifyTarget);
	tar += _workData[workDataIndex].verifyRemainderPrimorial;
	int n_offsets[MAX_SIEVE_WORKERS] = {0};
	static const int OFFSET_STACK_SIZE(16384);
	const uint64_t tupleSize(_parameters.primeTupleOffset.size());
	if (offsetStack == NULL) {
		offsetStack = new uint64_t*[MAX_SIEVE_WORKERS];
		offsetCount = new uint64_t*[MAX_SIEVE_WORKERS];
		for (int i(0) ; i < _parameters.sieveWorkers ; ++i) {
			offsetStack[i] = new uint64_t[OFFSET_STACK_SIZE];
			offsetCount[i] = new uint64_t[_parameters.maxIter];
			for (uint64_t segment(0) ; segment < _parameters.maxIter ; segment++)
				offsetCount[i][segment] = 0;
		}
	}

	// On Windows, caching these thread_local pointers on the stack makes a noticeable perf difference.
	uint64_t **offsets(offsetStack), **counts(offsetCount);
	const uint64_t precompLimit(_parameters.modPrecompute.size());

	uint64_t avxLimit(0);
	const uint64_t avxWidth(_cpuInfo.hasAVX2() ? 8 : 4);
	if (_cpuInfo.hasAVX()) {
		avxLimit = NUM_PRIMES_TO_2P32 - avxWidth;
		avxLimit -= (avxLimit - start_i) & (avxWidth - 1);  // Must be enough primes in range to use AVX
	}

	uint64_t nextRemainder[8];
	uint64_t nextRemainderIdx(8);
	for (uint64_t i(start_i) ; i < end_i ; i++) {
		const uint64_t p(_parameters.primes[i]);

		// Also update the offsets unless once only
		const bool onceOnly(i >= _sparseLimit);

		uint64_t invert[4];
		invert[0] = _parameters.inverts[i];

		// Compute the index, using precomputation speed up if available.
		uint64_t index, cnt(0), ps(0);
		if (i < precompLimit) {
			bool haveRemainder(false);
			if (nextRemainderIdx < avxWidth) {
				index = nextRemainder[nextRemainderIdx++];
				cnt = __builtin_clzll(p);
				ps = p << cnt;
				haveRemainder = true;
			}
			else if (i < avxLimit) {
				cnt = __builtin_clz((uint32_t) p);
				if (__builtin_clz((uint32_t) _parameters.primes[i + avxWidth - 1]) == cnt) {
					uint32_t ps32[8];
					for (uint64_t j(0) ; j < avxWidth; j++) {
						ps32[j] = (uint32_t) _parameters.primes[i + j] << cnt;
						nextRemainder[j] = _parameters.inverts[i + j];
					}
					if (_cpuInfo.hasAVX2()) rie_mod_1s_2p_8times(tar.get_mpz_t()->_mp_d, tar.get_mpz_t()->_mp_size, &ps32[0], cnt, &_parameters.modPrecompute[i], &nextRemainder[0]);
					else rie_mod_1s_2p_4times(tar.get_mpz_t()->_mp_d, tar.get_mpz_t()->_mp_size, &ps32[0], cnt, &_parameters.modPrecompute[i], &nextRemainder[0]);
					haveRemainder = true;
					index = nextRemainder[0];
					nextRemainderIdx = 1;
					cnt += 32;
					ps = (uint64_t) ps32[0] << 32;
				}
			}
			
			if (!haveRemainder) {
				cnt = __builtin_clzll(p);
				ps = p << cnt;
				const uint64_t remainder(rie_mod_1s_4p(tar.get_mpz_t()->_mp_d, tar.get_mpz_t()->_mp_size, ps, cnt, &_parameters.modPrecompute[i]));
				DBG_VERIFY(if (remainder >> cnt != mpz_tdiv_ui(tar.get_mpz_t(), p)) {std::cerr << "Remainder check fail " << (remainder >> cnt) << " != " << mpz_tdiv_ui(tar.get_mpz_t(), p) << std::endl; abort();});

				const uint64_t pa(ps - remainder);
				uint64_t r, nh, nl;
				umul_ppmm(nh, nl, pa, invert[0]);
				udiv_rnnd_preinv(r, nh, nl, ps, _parameters.modPrecompute[i]);
				index = r >> cnt;
				DBG_VERIFY(if (p < 0x100000000ull && (r >> cnt) != ((pa >> cnt)*invert[0]) % p) {std::cerr << "Remainder check fail" << std::endl; abort();});
			}
			DBG_VERIFY(({
				const uint64_t remainder(mpz_tdiv_ui(tar.get_mpz_t(), p)), pa(p - remainder);
				uint64_t q, nh, nl, indexCheck;
				umul_ppmm(nh, nl, pa, invert[0]);
				udiv_qrnnd(q, indexCheck, nh, nl, p);
				if (index != indexCheck) {std::cerr << "Index check fail, p = " << p << ", i = " << i << ", start_i = " << start_i << std::endl; abort();}
			}));
		}
		else {
			const uint64_t remainder(mpz_tdiv_ui(tar.get_mpz_t(), p)), pa(p - remainder);
			uint64_t q, nh, nl;
			umul_ppmm(nh, nl, pa, invert[0]);
			udiv_qrnnd(q, index, nh, nl, p);
		}

		invert[1] = (invert[0] << 1);
		if (invert[1] >= p) invert[1] -= p;
		invert[2] = invert[1] << 1;
		if (invert[2] >= p) invert[2] -= p;
		invert[3] = invert[1] + invert[2];
		if (invert[3] >= p) invert[3] -= p;

		// We use a macro here to ensure the compiler inlines the code, and also make it easier to early
		// out of the function completely if the current height has changed.
#define addToOffsets(j) { \
			if (!onceOnly) { \
				uint32_t* offsets = &_sieves[j].offsets[tupleSize*i]; \
				offsets[0] = index; \
				for (std::vector<uint64_t>::size_type f(1) ; f < _halfPrimeTupleOffset.size() ; f++) { \
					if (index < invert[_halfPrimeTupleOffset[f]]) index += p; \
					index -= invert[_halfPrimeTupleOffset[f]]; \
					offsets[f] = index; \
				} \
			} \
			else { \
				if (n_offsets[j] + _halfPrimeTupleOffset.size() >= OFFSET_STACK_SIZE) { \
					if (_workData[workDataIndex].verifyBlock.height != _currentHeight) { \
						return; \
					} \
					_putOffsetsInSegments(_sieves[j], offsets[j], counts[j], n_offsets[j]); \
					n_offsets[j] = 0; \
				} \
				if (index < _parameters.maxIncrements) { \
					offsets[j][n_offsets[j]++] = index; \
					counts[j][index >> _parameters.sieveBits]++; \
				} \
				for (std::vector<uint64_t>::size_type f(1) ; f < _halfPrimeTupleOffset.size() ; f++) { \
					if (index < invert[_halfPrimeTupleOffset[f]]) index += p; \
					index -= invert[_halfPrimeTupleOffset[f]]; \
					if (index < _parameters.maxIncrements) { \
						offsets[j][n_offsets[j]++] = index; \
						counts[j][index >> _parameters.sieveBits]++; \
					} \
				} \
			} \
		};
		addToOffsets(0);
		if (_parameters.sieveWorkers == 1) continue;

		uint64_t r;
#define recomputeRemainder(j) { \
			if (i < precompLimit && _primorialOffsetDiff[j - 1] < p) { \
				uint64_t nh, nl; \
				uint64_t os(_primorialOffsetDiff[j - 1] << cnt); \
				umul_ppmm(nh, nl, os, invert[0]); \
				udiv_rnnd_preinv(r, nh, nl, ps, _parameters.modPrecompute[i]); \
				r >>= cnt; \
				/* if (r != (_primorialOffsetDiff[j - 1]*invert[0]) % p) {  printf("Remainder check fail\n"); exit(-1); } */ \
			} \
			else { \
				uint64_t q, nh, nl; \
				umul_ppmm(nh, nl, _primorialOffsetDiff[j - 1], invert[0]); \
				udiv_qrnnd(q, r, nh, nl, p); \
			} \
		}
		recomputeRemainder(1);
		if (index < r) index += p;
		index -= r;
		addToOffsets(1);

		for (int j(2) ; j < _parameters.sieveWorkers ; j++) {
			if (_primorialOffsetDiff[j - 1] != _primorialOffsetDiff[j - 2])
				recomputeRemainder(j);
			if (index < r) index += p;
			index -= r;
			addToOffsets(j);
		}
	}

	if (end_i > _sparseLimit) {
		for (int j(0) ; j < _parameters.sieveWorkers ; j++) {
			if (n_offsets[j] > 0) {
				_putOffsetsInSegments(_sieves[j], offsets[j], counts[j], n_offsets[j]);
				n_offsets[j] = 0;
			}
		}
	}
}

void Miner::_processSieve(uint8_t *sieve, uint32_t* offsets, uint64_t start_i, uint64_t end_i) {
	const uint64_t tupleSize(_parameters.primeTupleOffset.size());
	uint32_t pending[PENDING_SIZE];
	uint64_t pending_pos(0);
	_initPending(pending);

	for (uint64_t i(start_i) ; i < end_i ; i++) {
		const uint32_t p(_parameters.primes[i]);
		for (uint64_t f(0) ; f < tupleSize; f++) {
			while (offsets[i*tupleSize + f] < _parameters.sieveSize) {
				_addToPending(sieve, pending, pending_pos, offsets[i*tupleSize + f]);
				offsets[i*tupleSize + f] += p;
			}
			offsets[i*tupleSize + f] -= _parameters.sieveSize;
		}
	}

	_termPending(sieve, pending);
}

void Miner::_processSieve6(uint8_t *sieve, uint32_t* offsets, uint64_t start_i, uint64_t end_i) {
	assert(_parameters.primeTupleOffset.size() == 6);
	uint32_t pending[PENDING_SIZE];
	uint64_t pending_pos(0);
	_initPending(pending);

	xmmreg_t offsetmax;
	offsetmax.m128 = _mm_set1_epi32(_parameters.sieveSize);
	
	assert((start_i & 1) == 0);
	assert((end_i & 1) == 0);

	for (uint64_t i(start_i) ; i < end_i ; i += 2) {
		xmmreg_t p1, p2, p3;
		xmmreg_t offset1, offset2, offset3, nextIncr1, nextIncr2, nextIncr3;
		xmmreg_t cmpres1, cmpres2, cmpres3;
		p1.m128 = _mm_set1_epi32(_parameters.primes[i]);
		p3.m128 = _mm_set1_epi32(_parameters.primes[i+1]);
		p2.m128 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(p1.m128), _mm_castsi128_ps(p3.m128), _MM_SHUFFLE(0, 0, 0, 0)));
		offset1.m128 = _mm_load_si128((__m128i const*) &offsets[i*6 + 0]);
		offset2.m128 = _mm_load_si128((__m128i const*) &offsets[i*6 + 4]);
		offset3.m128 = _mm_load_si128((__m128i const*) &offsets[i*6 + 8]);
		while (true) {
			cmpres1.m128 = _mm_cmpgt_epi32(offsetmax.m128, offset1.m128);
			cmpres2.m128 = _mm_cmpgt_epi32(offsetmax.m128, offset2.m128);
			cmpres3.m128 = _mm_cmpgt_epi32(offsetmax.m128, offset3.m128);
			const int mask1 = _mm_movemask_epi8(cmpres1.m128);
			const int mask2 = _mm_movemask_epi8(cmpres2.m128);
			const int mask3 = _mm_movemask_epi8(cmpres3.m128);
			if ((mask1 == 0) && (mask2 == 0) && (mask3 == 0)) break;
			_addRegToPending(sieve, pending, pending_pos, offset1, mask1);
			_addRegToPending(sieve, pending, pending_pos, offset2, mask2);
			_addRegToPending(sieve, pending, pending_pos, offset3, mask3);
			nextIncr1.m128 = _mm_and_si128(cmpres1.m128, p1.m128);
			nextIncr2.m128 = _mm_and_si128(cmpres2.m128, p2.m128);
			nextIncr3.m128 = _mm_and_si128(cmpres3.m128, p3.m128);
			offset1.m128 = _mm_add_epi32(offset1.m128, nextIncr1.m128);
			offset2.m128 = _mm_add_epi32(offset2.m128, nextIncr2.m128);
			offset3.m128 = _mm_add_epi32(offset3.m128, nextIncr3.m128);
		}
		offset1.m128 = _mm_sub_epi32(offset1.m128, offsetmax.m128);
		offset2.m128 = _mm_sub_epi32(offset2.m128, offsetmax.m128);
		offset3.m128 = _mm_sub_epi32(offset3.m128, offsetmax.m128);
		_mm_store_si128((__m128i*)&offsets[i*6 + 0], offset1.m128);
		_mm_store_si128((__m128i*)&offsets[i*6 + 4], offset2.m128);
		_mm_store_si128((__m128i*)&offsets[i*6 + 8], offset3.m128);
	}

	_termPending(sieve, pending);
}

void Miner::_runSieve(SieveInstance& sieve, uint32_t workDataIndex) {
	std::unique_lock<std::mutex> modLock(sieve.modLock, std::defer_lock);
	for (uint64_t loop(0) ; loop < _parameters.maxIter ; loop++) {
		if (_workData[workDataIndex].verifyBlock.height != _currentHeight)
			break;

		memset(sieve.sieve, 0, _parameters.sieveSize/8);

		// Align
		const uint64_t tupleSize(_parameters.primeTupleOffset.size());
		uint64_t start_i(_startingPrimeIndex);
		for ( ; (start_i & 1) != 0 ; start_i++) {
			const uint64_t pno(start_i);
			const uint32_t p(_parameters.primes[pno]);
			for (uint64_t f(0) ; f < tupleSize ; f++) {
				while (sieve.offsets[pno*tupleSize + f] < _parameters.sieveSize) {
					sieve.sieve[sieve.offsets[pno*tupleSize + f] >> 3] |= (1 << ((sieve.offsets[pno*tupleSize + f] & 7)));
					sieve.offsets[pno*tupleSize + f] += p;
				}
				sieve.offsets[pno*tupleSize + f] -= _parameters.sieveSize;
			}
		}

		// Main sieve
		if (tupleSize == 6)
			_processSieve6(sieve.sieve, sieve.offsets, start_i, _sparseLimit);
		else
			_processSieve(sieve.sieve, sieve.offsets, start_i, _sparseLimit);

		// Must now have all segments populated.
		if (loop == 0) modLock.lock();

		uint32_t pending[PENDING_SIZE];
		_initPending(pending);
		uint64_t pending_pos(0);
		for (uint64_t i(0) ; i < sieve.segmentCounts[loop] ; i++)
			_addToPending(sieve.sieve, pending, pending_pos, sieve.segmentHits[loop][i]);

		_termPending(sieve.sieve, pending);

		if (_workData[workDataIndex].verifyBlock.height != _currentHeight)
			break;

		primeTestWork w;
		w.testWork.n_indexes = 0;
		w.testWork.offsetId = sieve.id;
		w.testWork.loop = loop;
		w.type = TYPE_CHECK;
		w.workDataIndex = workDataIndex;
		
		bool stop(false);
		uint64_t *sieve64((uint64_t*) sieve.sieve);
		for (uint32_t b(0) ; !stop && b < _parameters.sieveWords ; b++) {
			uint64_t sb(~sieve64[b]);

			while (sb != 0) {
				const uint32_t lowsb(__builtin_ctzll(sb)), i((b*64) + lowsb);
				sb &= sb - 1;

				w.testWork.indexes[w.testWork.n_indexes] = i;
				w.testWork.n_indexes++;

				if (w.testWork.n_indexes == WORK_INDEXES) {
					// Low overhead but still often enough
					if (_workData[workDataIndex].verifyBlock.height != _currentHeight) {
						stop = true;
						break;
					}

					_verifyWorkQueue.push_back(w);
					w.testWork.n_indexes = 0;
					_workData[workDataIndex].outstandingTests++;
				}
			}
		}

		if (_workData[workDataIndex].verifyBlock.height != _currentHeight) break;

		if (w.testWork.n_indexes > 0) {
			_verifyWorkQueue.push_back(w);
			_workData[workDataIndex].outstandingTests++;
		}
	}
}

bool Miner::_testPrimesIspc(uint32_t indexes[WORK_INDEXES], uint32_t is_prime[WORK_INDEXES], const mpz_class &ploop, mpz_class &candidate) {
	uint32_t M[WORK_INDEXES * MAX_N_SIZE], bits(0), N_Size;
	uint32_t *mp(&M[0]);
	for (uint32_t i(0); i < WORK_INDEXES; ++i) {
		candidate = _primorial*indexes[i];
		candidate += ploop;

		if (bits == 0) {
			bits = mpz_sizeinbase(candidate.get_mpz_t(), 2);
			N_Size = (bits >> 5) + ((bits & 0x1f) > 0);
			if (N_Size > MAX_N_SIZE) return false;
		}
		else assert(bits == mpz_sizeinbase(candidate.get_mpz_t(), 2));

		memcpy(mp, candidate.get_mpz_t()->_mp_d, N_Size*4);
		mp += N_Size;
	}

	fermatTest(N_Size, WORK_INDEXES, M, is_prime, _cpuInfo.hasAVX512());
	return true;
}

void Miner::_verifyThread() {
/* Check for a prime cluster. Uses the fermat test - jh's code noted that it is
slightly faster. Could do an MR test as a follow-up, but the server can do this
too for the one-in-a-whatever case that Fermat is wrong. */
	mpz_class candidate, candidateOffset, ploop;

	while (_running) {
		primeTestWork job;
		if (!_modWorkQueue.pop_front_if_not_empty(job)) {
			job = _verifyWorkQueue.pop_front();
		}
		const auto startTime(std::chrono::high_resolution_clock::now());
		
		if (job.type == TYPE_MOD) {
			_updateRemainders(job.workDataIndex, job.modWork.start, job.modWork.end);
			_workDoneQueue.push_back(-int64_t(job.modWork.start));
			_modTime += std::chrono::duration_cast<decltype(_modTime)>(std::chrono::high_resolution_clock::now() - startTime);
			continue;
		}
		
		if (job.type == TYPE_SIEVE) {
			_runSieve(_sieves[job.sieveWork.sieveId], job.workDataIndex);
			_workDoneQueue.push_back(-1);
			const auto dt(std::chrono::duration_cast<decltype(_sieveTime)>(std::chrono::high_resolution_clock::now() - startTime));
			_sieveTime += dt;
			continue;
		}
		
		if (job.type == TYPE_CHECK) { // fallthrough: job.type == TYPE_CHECK
			mpz_mul_ui(ploop.get_mpz_t(), _primorial.get_mpz_t(), job.testWork.loop*_parameters.sieveSize);
			ploop += _workData[job.workDataIndex].verifyRemainderPrimorial;
			ploop += _workData[job.workDataIndex].verifyTarget;
			mpz_add_ui(ploop.get_mpz_t(), ploop.get_mpz_t(), _primorialOffsetDiffToFirst[job.testWork.offsetId]);

			bool firstTestDone(false);
			if (_cpuInfo.hasAVX2() && _manager->options().enableAvx2() && job.testWork.n_indexes == WORK_INDEXES) {
				uint32_t isPrime[WORK_INDEXES];
				firstTestDone = _testPrimesIspc(job.testWork.indexes, isPrime, ploop, candidate);
				if (firstTestDone) {
					job.testWork.n_indexes = 0;
					for (uint32_t i(0) ; i < WORK_INDEXES ; i++) {
						DBG_VERIFY(({
							if (isPrimeFermat(candidate)) assert(isPrime[i]);
							else assert(!isPrime[i]);
						}));
						_manager->incTupleCount(0);
						if (isPrime[i])
							job.testWork.indexes[job.testWork.n_indexes++] = job.testWork.indexes[i];
					}
				}
			}

			for (uint32_t idx(0) ; idx < job.testWork.n_indexes ; idx++) {
				if (_currentHeight != _workData[job.workDataIndex].verifyBlock.height) break;

				uint8_t tupleLength(0);
				candidate = _primorial*job.testWork.indexes[idx];
				candidate += ploop;
				
				if (!firstTestDone) {
					_manager->incTupleCount(tupleLength);
					if (!isPrimeFermat(candidate)) continue;
				}

				candidateOffset = candidate - _workData[job.workDataIndex].verifyTarget; // offset = tested - target
				
				tupleLength++;
				_manager->incTupleCount(tupleLength);
				uint16_t offsetSum(0);
				// Note start at 1 - we've already tested bias 0
				for (std::vector<uint64_t>::size_type i(1) ; i < _parameters.primeTupleOffset.size() ; i++) {
					offsetSum += _parameters.primeTupleOffset[i];
					mpz_add_ui(candidate.get_mpz_t(), candidate.get_mpz_t(), _parameters.primeTupleOffset[i]);
					if (isPrimeFermat(mpz_class(candidate))) {
						tupleLength++;
						_manager->incTupleCount(tupleLength);
					}
					else if (!_parameters.solo) {
						int candidatesRemaining(5 - i);
						if ((tupleLength + candidatesRemaining) < 4) break;
					}
					else break;
				}
				
				if (_parameters.solo) {
					if (tupleLength < _parameters.tupleLengthMin) continue;
				}
				else if (tupleLength < 4) continue;
	
				// Generate nOffset and submit
				for (uint32_t d(0) ; d < (uint32_t) std::min(32/((uint32_t) sizeof(mp_limb_t)), (uint32_t) candidateOffset.get_mpz_t()->_mp_size) ; d++)
					*(mp_limb_t*) (_workData[job.workDataIndex].verifyBlock.bh.nOffset + d*sizeof(mp_limb_t)) = candidateOffset.get_mpz_t()->_mp_d[d];
				_workData[job.workDataIndex].verifyBlock.primes = tupleLength;
				if (_manager->options().mode() == "Benchmark") {
					mpz_class n(candidate - offsetSum);
					std::cout << "Found n = " << n << std::endl;
					if (_manager->options().tuplesFile() != "None") {
						_tupleFileLock.lock();
						std::ofstream file(_manager->options().tuplesFile(), std::ios::app);
						if (file)
							file << static_cast<uint16_t>(tupleLength) << "-tuple: " << n << std::endl;
						else
							std::cerr << "Unable to write file " << _manager->options().tuplesFile() << " in order to write a tuple :|" << std::endl;
						_tupleFileLock.unlock();
					}
				}
				_manager->submitWork(_workData[job.workDataIndex].verifyBlock);
			}
			
			_workDoneQueue.push_back(job.workDataIndex);
			_verifyTime += std::chrono::duration_cast<decltype(_verifyTime)>(std::chrono::high_resolution_clock::now() - startTime);
		}
	}
}

void Miner::_getTargetFromBlock(mpz_class &target, const WorkData &block) {
	std::vector<uint8_t> powHash(block.bh.powHash());
	target = 1;
	target <<= ZEROS_BEFORE_HASH;
	for (uint64_t i(0) ; i < 256 ; i++) {
		target <<= 1;
		if ((powHash[i/8] >> (i % 8)) & 1)
			target.get_mpz_t()->_mp_d[0]++;
	}
	
	const uint64_t trailingZeros(block.difficulty - 1 - ZEROS_BEFORE_HASH - 256);
	target <<= trailingZeros;
}

void Miner::_processOneBlock(uint32_t workDataIndex, bool isNewHeight) {
	mpz_class target, candidate, remainderPrimorial;
	_getTargetFromBlock(target, _workData[workDataIndex].verifyBlock);
	if (_running) {
		// find first offset where target%primorial = _parameters.primorialOffset
		remainderPrimorial = target % _primorial;
		mpz_abs(remainderPrimorial.get_mpz_t(), remainderPrimorial.get_mpz_t());
		remainderPrimorial = (_primorial - remainderPrimorial) % _primorial;
		mpz_abs(remainderPrimorial.get_mpz_t(), remainderPrimorial.get_mpz_t());
		remainderPrimorial += _parameters.primorialOffsets[0];
		candidate = target + remainderPrimorial;
		
		_workData[workDataIndex].verifyTarget = target;
		_workData[workDataIndex].verifyRemainderPrimorial = remainderPrimorial;
		
		for (int i(0) ; i < _parameters.sieveWorkers ; i++)
			for (uint64_t j(0) ; j < _parameters.maxIter; j++) _sieves[i].segmentCounts[j] = 0;
		
		primeTestWork wi;
		wi.type = TYPE_MOD;
		wi.workDataIndex = workDataIndex;
		primeTestWork wd;
		wd.type = TYPE_DUMMY;
		int32_t nModWorkers(0), nLowModWorkers(0);
		
		const uint32_t curWorkOut(_verifyWorkQueue.size());
		const uint64_t incr(_nPrimes/(_parameters.threads*8));
		for (auto base(_startingPrimeIndex) ; base < _nPrimes ; base += incr) {
			uint64_t lim(std::min(_nPrimes, base + incr));
			wi.modWork.start = base;
			wi.modWork.end = lim;
			_modWorkQueue.push_back(wi);
			_verifyWorkQueue.push_front(wd);  // To ensure a thread wakes up to grab the mod work.
			if (wi.modWork.start < _sparseLimit) nLowModWorkers++;
			else nModWorkers++;
		}
		while (nLowModWorkers > 0) {
			const int64_t i(_workDoneQueue.pop_front());
			if (i >= 0) _workData[i].outstandingTests--;
			else {
				if (uint64_t(-i) < _sparseLimit) nLowModWorkers--;
				else nModWorkers--;
			}
		}

		assert(_workData[workDataIndex].outstandingTests == 0);

		wi.type = TYPE_SIEVE;
		for (int i(0); i < _parameters.sieveWorkers; ++i) {
			wi.sieveWork.sieveId = i;
			_sieves[i].modLock.lock();
			_verifyWorkQueue.push_front(wi);
		}
		int nSieveWorkers(_parameters.sieveWorkers);
		
		while (nModWorkers > 0) {
			const int64_t i(_workDoneQueue.pop_front());
			if (i >= 0) _workData[i].outstandingTests--;
			else if (i == -1) nSieveWorkers--;
			else nModWorkers--;
		}
		for (int i(0) ; i < _parameters.sieveWorkers; ++i) _sieves[i].modLock.unlock();

		uint32_t minWorkOut(std::min(curWorkOut, _verifyWorkQueue.size()));
		while (nSieveWorkers > 0) {
			const int workId(_workDoneQueue.pop_front());
			if (workId == -1) nSieveWorkers--;
			else _workData[workId].outstandingTests--;
			minWorkOut = std::min(minWorkOut, _verifyWorkQueue.size());
		}

		if (_currentHeight == _workData[workDataIndex].verifyBlock.height && !isNewHeight) {
			DBG(std::cout << "Min work outstanding during sieving: " << minWorkOut << std::endl;);
			if (curWorkOut > _maxWorkOut - _parameters.threads*2) {
				// If we are acheiving our work target, then adjust it towards the amount
				// required to maintain a healthy minimum work queue length.
				if (minWorkOut == 0) {
					// Need more, but don't know how much, try adding some.
					_maxWorkOut += 4*_parameters.threads*_parameters.sieveWorkers;
				}
				else {
					// Adjust towards target of min work = 4*threads
					const uint32_t targetMaxWork((_maxWorkOut - minWorkOut) + 8*_parameters.threads);
					_maxWorkOut = (_maxWorkOut + targetMaxWork)/2;
				}
			}
			else if (minWorkOut > 4u*_parameters.threads) {
				// Didn't make the target, but also didn't run out of work.  Can still adjust target.
				const uint32_t targetMaxWork((curWorkOut - minWorkOut) + 10*_parameters.threads);
				_maxWorkOut = (_maxWorkOut + targetMaxWork)/2;
			}
			else if (minWorkOut == 0 && curWorkOut > 0) {
				// Warn possible CPU Underuse
				static int allowedFails(5);
				if (--allowedFails == 0) {
					allowedFails = 5;
					DBG(std::cout << "Unable to generate enough verification work to keep threads busy." << std::endl;
					    std::cout << "PTL = " << _parameters.primeTableLimit << ", sieve workers = " << _parameters.sieveWorkers << std::endl;);
				}
			}
			_maxWorkOut = std::min(_maxWorkOut, _workDoneQueue.size() - 9*_parameters.threads);
			DBG(std::cout << "Work target before starting next block now: " << _maxWorkOut << std::endl;);
		}
	}
}

void Miner::process(WorkData block) {
	if (!_masterExists) {
		_masterLock.lock();
		if (!_masterExists) {
			_masterExists = true;
			isMaster = true;
		}
		_masterLock.unlock();
	}
	
	if (!isMaster) {
		_verifyThread();
		usleep(1000000);
		return;
	}
	
	uint32_t workDataIndex(0), oldHeight(0);
	_workData[workDataIndex].verifyBlock = block;
	
	do {
		_modTime = _modTime.zero();
		_sieveTime = _sieveTime.zero();
		_verifyTime = _verifyTime.zero();
		
		_processOneBlock(workDataIndex, oldHeight != _workData[workDataIndex].verifyBlock.height);
		oldHeight = _workData[workDataIndex].verifyBlock.height;

		while (_workData[workDataIndex].outstandingTests > _maxWorkOut)
			_workData[_workDoneQueue.pop_front()].outstandingTests--;

		workDataIndex = (workDataIndex + 1) % WORK_DATAS;
		while (_workData[workDataIndex].outstandingTests > 0)
			_workData[_workDoneQueue.pop_front()].outstandingTests--;

		DBG(std::cout << "Block timing: " << _modTime.count() << ", " << _sieveTime.count() << ", " << _verifyTime.count() << "  Tests out: " << _workData[0].outstandingTests << ", " << _workData[1].outstandingTests << std::endl;);

	} while (_manager->getWork(_workData[workDataIndex].verifyBlock));

	for (workDataIndex = 0 ; workDataIndex < WORK_DATAS ; workDataIndex++) {
		while (_workData[workDataIndex].outstandingTests > 0)
			_workData[_workDoneQueue.pop_front()].outstandingTests--;
	}
}
