#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/*
* FNV-1a (Fowler, Noll and Vo), 64-bit: a simple, well-spread hash fed one byte at a time. Each byte is mixed in with an
* exclusive or and then spread over all 64 bits by a multiplication with a large prime, so changing any single bit of
* the input changes the result completely. Not for security (it is easy to construct collisions on purpose), but ideal
* for "did anything change?" checks: shadow caster lists, and the simulation state in determinism tests.
*
* Numbers are hashed by their exact bits, so 0 and -0, or two floats one last bit apart, hash differently. Hash a struct
* field by field, never as one block of memory: the padding bytes between fields hold arbitrary values.
*/
struct Fnv1aHash {
    static constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    static constexpr std::uint64_t prime = 1099511628211ull;

    std::uint64_t value = offsetBasis;

    void addBytes(const void* data, std::size_t byteCount) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);

        for (std::size_t i = 0; i < byteCount; ++i) {
            value = (value ^ bytes[i]) * prime;
        }
    }

    // One number or flag, by its bits (a bool as one byte, 0 or 1).
    template <typename Number>
        requires std::is_arithmetic_v<Number>
    void add(Number number) {
        addBytes(&number, sizeof(number));
    }
};

/*
* SplitMix64's finaliser (Steele, Lea and Flood 2014): shifts, xors and two multiplications that make every input bit
* affect every output bit about half the time. FNV-1a's last multiplication carries a byte's change only upwards, so
* hashes of nearby inputs differ little in their top bits; mix a hash before using its bits as random numbers.
*/
constexpr std::uint64_t mixBits(std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
