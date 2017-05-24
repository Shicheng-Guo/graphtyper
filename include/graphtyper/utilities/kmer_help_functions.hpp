#pragma once

#include <seqan/basic.h>
#include <seqan/sequence.h>

#include <graphtyper/graph/graph.hpp>
#include <graphtyper/index/kmer_label.hpp>
#include <graphtyper/index/rocksdb.hpp>
#include <graphtyper/typer/genotype_paths.hpp>
#include <graphtyper/utilities/type_conversions.hpp>

namespace gyper
{

template <typename TSequence>
std::size_t get_num_kmers(TSequence const & dna);
template <typename TSequence>
TSequence get_ith_kmer(TSequence const & dna, std::size_t i);

template <typename TSequence>
uint32_t read_offset(TSequence const & dna);

template <typename TSeq>
std::vector<std::vector<KmerLabel> >
query_index(TSeq const & read);

template <typename TSeq>
std::vector<std::vector<KmerLabel> >
query_index_hamming_distance1(TSeq const & read);

template <typename TSeq>
std::vector<std::vector<KmerLabel> >
query_index_hamming_distance1_without_index(TSeq const & read);

} // namespace gyper
